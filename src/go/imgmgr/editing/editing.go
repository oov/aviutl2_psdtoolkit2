package editing

import (
	"bytes"
	"context"
	"encoding/json"
	"image"
	"image/png"
	"math"
	"path/filepath"
	"time"

	"github.com/oov/downscale"
	"github.com/pkg/errors"
	"golang.org/x/image/draw"

	"psdtoolkit/img"
	"psdtoolkit/imgmgr/source"
	"psdtoolkit/warn"
)

const (
	thumbnailSize = 48
	textureSize   = 512

	limit = int(textureSize/thumbnailSize) * int(textureSize/thumbnailSize)
)

// Thumbnailer handles delayed thumbnail generation for an image.
type Thumbnailer struct {
	editing *Editing
	index   int
	t       *time.Timer
}

func makeThumbnail(src image.Image) (*image.NRGBA, error) {
	rect := src.Bounds()
	dx, dy := float64(rect.Dx()), float64(rect.Dy())
	f := thumbnailSize / math.Max(dx, dy)
	rect = image.Rect(0, 0, int(dx*f), int(dy*f))
	switch src0 := src.(type) {
	case *image.RGBA:
		tmp := image.NewRGBA(rect)
		if err := downscale.RGBAGamma(context.Background(), tmp, src0, 2.2); err != nil {
			return nil, err
		}
		src = tmp
	case *image.NRGBA:
		tmp := image.NewNRGBA(rect)
		if err := downscale.NRGBAGamma(context.Background(), tmp, src0, 2.2); err != nil {
			return nil, err
		}
		src = tmp
	default:
		return nil, errors.Errorf("unsupported image type %t", src)
	}

	r := image.NewNRGBA(rect)
	draw.Draw(r, rect, src, image.Pt(0, 0), draw.Over)
	return r, nil
}

// Update schedules a thumbnail update after a delay.
// The thumbnail generation runs in a separate goroutine.
func (t *Thumbnailer) Update(img image.Image) {
	if t.t != nil {
		t.t.Stop()
	}
	index := t.index
	t.t = time.AfterFunc(500*time.Millisecond, func() {
		thumb, err := makeThumbnail(img)
		if err != nil {
			// TODO: report error
			return
		}
		t.editing.Requests <- UpdateThumbnailReq{
			Index:     index,
			Thumbnail: thumb,
		}
	})
}

// Item represents an image being edited.
type Item struct {
	DisplayName string
	Image       *img.Image
	Tag         int
	LatestState string
	Thumbnail   *image.NRGBA
	ViewState   *img.ViewState // View settings (zoom, scroll) for this image
}

// Snapshot is a read-only copy of the editing state for GUI consumption.
type Snapshot struct {
	Items         []Item
	SelectedIndex int
	SplitterWidth float32 // 0 means not set (use default)
}

// Editing manages the list of images being edited.
// It runs as a single goroutine (actor model) and receives requests via the Requests channel.
// No mutex is needed because only the Run goroutine accesses the internal state.
type Editing struct {
	srcs *source.Sources

	// Internal state (only accessed by Run goroutine)
	images        []Item
	selectedIndex int

	// GUI global settings
	SplitterWidth float32 // Splitter position in DIP

	// Requests is the channel for receiving requests.
	// Use a buffered channel to avoid blocking callers.
	Requests chan any

	// OnChange is called when the editing state changes.
	// It is called from the Run goroutine.
	// The GUI should post the snapshot to its queue for processing.
	OnChange func(Snapshot)
}

// New creates a new Editing instance.
func New(srcs *source.Sources) *Editing {
	return &Editing{
		srcs:     srcs,
		Requests: make(chan any, 64),
	}
}

// Run starts the actor loop. It should be called in a separate goroutine.
func (ed *Editing) Run(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case req := <-ed.Requests:
			ed.handle(req)
		}
	}
}

func (ed *Editing) handle(req any) {
	switch r := req.(type) {
	case AddFileReq:
		idx, err := ed.addFile(r.FilePath, r.Tag)
		if r.Reply != nil {
			r.Reply <- AddFileResp{idx, err}
		}
		if err == nil && idx == -1 {
			ed.notifyChange()
		}

	case SerializeReq:
		state, err := ed.serialize()
		r.Reply <- SerializeResp{state, err}

	case DeserializeReq:
		err := ed.deserialize(r.State)
		if r.Reply != nil {
			r.Reply <- DeserializeResp{err}
		}
		if err == nil {
			ed.notifyChange()
		}

	case UpdateTagStateReq:
		needRefresh, err := ed.updateTagState(r.FilePath, r.Tag, r.State)
		if r.Reply != nil {
			r.Reply <- UpdateTagStateResp{needRefresh, err}
		}
		if needRefresh {
			ed.notifyChange()
		}

	case DeleteReq:
		ed.delete(r.Index)
		ed.notifyChange()

	case ClearReq:
		ed.clear()
		ed.notifyChange()

	case SelectReq:
		if r.Index != ed.selectedIndex && r.Index >= 0 && r.Index < len(ed.images) {
			ed.selectedIndex = r.Index
			ed.notifyChange()
		}

	case TouchReq:
		ed.touch()

	case GetSnapshotReq:
		r.Reply <- ed.makeSnapshot()

	case GetSelectedImageReq:
		r.Reply <- ed.getSelectedImage()

	case UpdateThumbnailReq:
		if r.Index >= 0 && r.Index < len(ed.images) {
			ed.images[r.Index].Thumbnail = r.Thumbnail
			ed.notifyChange()
		}

	case UpdateViewStateReq:
		if r.Index >= 0 && r.Index < len(ed.images) {
			ed.images[r.Index].ViewState = r.ViewState
			// No need to notify change for view state updates
		}

	case SetSplitterWidthReq:
		ed.SplitterWidth = r.Width

	case GetSplitterWidthReq:
		r.Reply <- ed.SplitterWidth
	}
}

func (ed *Editing) notifyChange() {
	if ed.OnChange != nil {
		ed.OnChange(ed.makeSnapshot())
	}
}

func (ed *Editing) makeSnapshot() Snapshot {
	items := make([]Item, len(ed.images))
	copy(items, ed.images)
	return Snapshot{
		Items:         items,
		SelectedIndex: ed.selectedIndex,
		SplitterWidth: ed.SplitterWidth,
	}
}

// --- Internal methods (called from handle) ---

func (ed *Editing) addFile(filePath string, tag int) (int, error) {
	if tag != 0 {
		for idx, item := range ed.images {
			if item.Tag == tag {
				return idx, nil
			}
		}
	}
	if len(ed.images) == limit {
		return -1, errors.Errorf("too many images")
	}
	img, err := ed.srcs.NewImage(filePath)
	if err != nil {
		return -1, errors.Wrapf(err, "editing: failed to load %q", filePath)
	}
	ed.images = append(ed.images, Item{
		DisplayName: filepath.Base(*img.FilePath),
		Image:       img,
		Tag:         tag,
	})
	ed.selectedIndex = len(ed.images) - 1
	return -1, nil
}

func (ed *Editing) delete(index int) {
	if index < 0 || index >= len(ed.images) {
		return
	}
	copy(ed.images[index:], ed.images[index+1:])
	ed.images[len(ed.images)-1] = Item{}
	ed.images = ed.images[:len(ed.images)-1]

	if len(ed.images) == 0 {
		ed.selectedIndex = 0
	} else {
		if index != 0 && index <= ed.selectedIndex {
			ed.selectedIndex--
		}
	}
}

func (ed *Editing) clear() {
	ed.images = nil
	ed.selectedIndex = 0
}

func (ed *Editing) touch() {
	for _, item := range ed.images {
		item.Image.Touch()
	}
}

func (ed *Editing) getSelectedImage() GetSelectedImageResp {
	if ed.selectedIndex < 0 || ed.selectedIndex >= len(ed.images) {
		return GetSelectedImageResp{}
	}
	item := ed.images[ed.selectedIndex]
	return GetSelectedImageResp{
		Image:       item.Image,
		DisplayName: item.DisplayName,
		LatestState: item.LatestState,
	}
}

// serializeData holds per-image serialization data
type serializeData struct {
	Image     *img.ProjectState
	Tag       int
	Thumbnail []byte
}

// serializeRoot is the new root structure for serialization
// Version 0 or missing: legacy format ([]serializeData)
// Version 1: new format with global settings
type serializeRoot struct {
	Version       int             `json:"version"`
	SplitterWidth float32         `json:"splitterWidth,omitempty"`
	Images        []serializeData `json:"images"`
}

func (ed *Editing) serialize() (string, error) {
	var images []serializeData
	for _, item := range ed.images {
		var thumb []byte
		if item.Thumbnail != nil {
			b := bytes.NewBufferString("")
			if err := png.Encode(b, item.Thumbnail); err == nil {
				thumb = b.Bytes()
			}
		}
		ps := item.Image.SerializeProject()
		// Include view state if available
		ps.ViewState = item.ViewState
		images = append(images, serializeData{
			Image:     ps,
			Tag:       item.Tag,
			Thumbnail: thumb,
		})
	}

	root := serializeRoot{
		Version:       1,
		SplitterWidth: ed.SplitterWidth,
		Images:        images,
	}

	b := bytes.NewBufferString("")
	if err := json.NewEncoder(b).Encode(root); err != nil {
		return "", err
	}
	return b.String(), nil
}

func (ed *Editing) deserialize(state string) error {
	if state == "" {
		ed.clear()
		return nil
	}

	// Try to decode as new format first
	var root serializeRoot
	decoder := json.NewDecoder(bytes.NewReader([]byte(state)))
	if err := decoder.Decode(&root); err != nil {
		return err
	}

	// Check if this is the new format (has version field)
	var srz []serializeData
	if root.Version >= 1 {
		// New format
		srz = root.Images
		ed.SplitterWidth = root.SplitterWidth
	} else {
		// Legacy format: the JSON is an array, not an object
		// Re-decode as legacy format
		if err := json.NewDecoder(bytes.NewReader([]byte(state))).Decode(&srz); err != nil {
			return err
		}
		// Keep current splitter width for legacy data
	}

	if len(srz) > limit {
		return errors.Errorf("too many images: %d > %d", len(srz), limit)
	}

	ed.clear()
	var wr warn.Warning

	for _, d := range srz {
		img, err := ed.srcs.NewImage(d.Image.FilePath)
		if err != nil {
			wr = append(wr, errors.Wrapf(err, "editing: cannot load %q", d.Image.FilePath))
			continue
		}

		it := Item{
			DisplayName: filepath.Base(*img.FilePath),
			Image:       img,
			Tag:         d.Tag,
			ViewState:   d.Image.ViewState, // Restore view state (may be nil for old data)
		}

		if len(d.Thumbnail) > 0 {
			if decoded, err := png.Decode(bytes.NewReader(d.Thumbnail)); err == nil {
				it.Thumbnail = image.NewNRGBA(decoded.Bounds())
				draw.Draw(it.Thumbnail, it.Thumbnail.Rect, decoded, image.Point{}, draw.Over)
			}
		}

		if w, err := img.DeserializeProject(d.Image); err != nil {
			wr = append(wr, errors.Wrapf(err, "editing: failed to deserialize on %q", d.Image.FilePath))
		} else if w != nil {
			wr = append(wr, w...)
		}

		ed.images = append(ed.images, it)
	}

	if len(ed.images) > 0 {
		ed.selectedIndex = len(ed.images) - 1
	} else {
		ed.selectedIndex = 0
	}

	if wr != nil {
		ed.srcs.Logger.Println(wr)
	}

	return nil
}

func (ed *Editing) updateTagState(filePath string, tag int, state string) (bool, error) {
	// Check if file with tag already exists
	var existingIndex = -1
	if tag != 0 {
		for idx, item := range ed.images {
			if item.Tag == tag {
				existingIndex = idx
				break
			}
		}
	}

	needRefresh := false

	if existingIndex >= 0 {
		// Update existing item's latest state
		ed.images[existingIndex].LatestState = state
	} else {
		// Load new image
		if len(ed.images) >= limit {
			return false, errors.Errorf("too many images")
		}

		img, err := ed.srcs.NewImage(filePath)
		if err != nil {
			return false, errors.Wrapf(err, "editing: failed to load %q", filePath)
		}

		ed.images = append(ed.images, Item{
			DisplayName: filepath.Base(*img.FilePath),
			Image:       img,
			Tag:         tag,
			LatestState: state,
		})
		ed.selectedIndex = len(ed.images) - 1

		// Deserialize state to the newly added image
		if state != "" {
			ed.images[ed.selectedIndex].Image.Deserialize(state)
		}
		needRefresh = true
	}

	return needRefresh, nil
}

// --- Client API (synchronous helpers for IPC) ---

// AddFile adds a file synchronously and waits for the result.
func (ed *Editing) AddFile(filePath string, tag int) (int, error) {
	reply := make(chan AddFileResp, 1)
	ed.Requests <- AddFileReq{filePath, tag, reply}
	resp := <-reply
	return resp.Index, resp.Err
}

// Serialize serializes the editing state synchronously.
func (ed *Editing) Serialize() (string, error) {
	reply := make(chan SerializeResp, 1)
	ed.Requests <- SerializeReq{reply}
	resp := <-reply
	return resp.State, resp.Err
}

// Deserialize deserializes the editing state synchronously.
func (ed *Editing) Deserialize(state string) error {
	reply := make(chan DeserializeResp, 1)
	ed.Requests <- DeserializeReq{state, reply}
	resp := <-reply
	return resp.Err
}

// UpdateTagState updates the tag state synchronously.
func (ed *Editing) UpdateTagState(filePath string, tag int, state string) (bool, error) {
	reply := make(chan UpdateTagStateResp, 1)
	ed.Requests <- UpdateTagStateReq{filePath, tag, state, reply}
	resp := <-reply
	return resp.NeedRefresh, resp.Err
}

// Clear clears all items asynchronously.
func (ed *Editing) Clear() {
	ed.Requests <- ClearReq{}
}

// Delete deletes an item at the specified index asynchronously.
func (ed *Editing) Delete(index int) {
	ed.Requests <- DeleteReq{index}
}

// Select changes the selected index asynchronously.
func (ed *Editing) Select(index int) {
	ed.Requests <- SelectReq{index}
}

// Touch touches all images asynchronously.
func (ed *Editing) Touch() {
	ed.Requests <- TouchReq{}
}

// GetSnapshot gets a snapshot of the current state synchronously.
func (ed *Editing) GetSnapshot() Snapshot {
	reply := make(chan Snapshot, 1)
	ed.Requests <- GetSnapshotReq{reply}
	return <-reply
}

// GetSelectedImage gets the selected image information synchronously.
func (ed *Editing) GetSelectedImage() GetSelectedImageResp {
	reply := make(chan GetSelectedImageResp, 1)
	ed.Requests <- GetSelectedImageReq{reply}
	return <-reply
}

// CreateThumbnailer creates a Thumbnailer for the specified index.
func (ed *Editing) CreateThumbnailer(index int) *Thumbnailer {
	return &Thumbnailer{
		editing: ed,
		index:   index,
	}
}
