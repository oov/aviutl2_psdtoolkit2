package gui

import (
	"bytes"
	"context"
	"image/png"
	"time"

	"github.com/golang-ui/nuklear/nk"
	"github.com/pkg/errors"

	"psdtoolkit/clipboard"
	"psdtoolkit/gc"
	"psdtoolkit/gui/layerview"
	"psdtoolkit/gui/mainview"
	"psdtoolkit/gui/tabview"
	"psdtoolkit/img"
	"psdtoolkit/imgmgr/editing"
	"psdtoolkit/locale"
	"psdtoolkit/nkhelper"
	"psdtoolkit/ods"
)

const (
	winWidth  = 1024
	winHeight = 768
)

// GUI manages the graphical user interface.
type GUI struct {
	queue chan func()
	ready chan struct{}

	window  *window
	context *nk.Context

	editing *editing.Editing

	// Current snapshot (updated via OnChange callback)
	snapshot editing.Snapshot

	// Track the last selected image to detect actual selection changes
	lastSelectedImg *img.Image

	// Thumbnail cache (GUI thread only)
	thumbCache *ThumbnailCache

	cancelRender context.CancelFunc

	img         *img.Image
	thumbnailer *editing.Thumbnailer

	tabView   *tabview.TabView
	layerView *layerview.LayerView
	mainView  *mainview.MainView

	font struct {
		Main       *font
		MainHandle *nk.UserFont

		Symbol       *font
		SymbolHandle *nk.UserFont
	}

	lastDPI    uint32
	symbolFont []byte

	SendEditingImageState func(path, state string) error
	ExportFaviewSlider    func(path, sliderName string, names, values []string, selectedIndex int) error
	ExportLayerNames      func(path string, names, values []string, selectedIndex int) error
	DropFiles             func(filenames []string)
}

// New creates a new GUI instance.
func New(ed *editing.Editing) *GUI {
	g := &GUI{
		// Buffered channel is required to avoid deadlock.
		queue:      make(chan func(), 64),
		ready:      make(chan struct{}),
		editing:    ed,
		thumbCache: NewThumbnailCache(),
	}

	// Set up change notification
	ed.OnChange = func(snap editing.Snapshot) {
		g.queue <- func() {
			g.snapshot = snap
			g.onSnapshotChange()
		}
	}

	return g
}

// onSnapshotChange is called when the editing state changes.
// Only trigger changeSelectedImage if the actual selected image changed,
// not just for thumbnail updates which would cause an infinite loop.
func (g *GUI) onSnapshotChange() {
	var currentImg *img.Image
	if g.snapshot.SelectedIndex >= 0 && g.snapshot.SelectedIndex < len(g.snapshot.Items) {
		currentImg = g.snapshot.Items[g.snapshot.SelectedIndex].Image
	}
	if currentImg != g.lastSelectedImg {
		g.lastSelectedImg = currentImg
		g.changeSelectedImage()
	}
}

// AddFile adds a file (called from GUI thread, e.g., drag-drop).
func (g *GUI) AddFile(path string, tag int) error {
	gc.EnterCS()
	go g.do(func() error { gc.LeaveCS(); return nil })

	_, err := g.editing.AddFile(path, tag)
	return err
}

// AddFileSync adds a file synchronously (called from IPC thread).
func (g *GUI) AddFileSync(path string, tag int) error {
	_, err := g.editing.AddFile(path, tag)
	return err
}

// UpdateTagStateSync updates the tag state synchronously (called from IPC thread).
func (g *GUI) UpdateTagStateSync(path string, tag int, state string) error {
	_, err := g.editing.UpdateTagState(path, tag, state)
	return err
}

// ClearFiles clears all files (called from IPC thread).
func (g *GUI) ClearFiles() error {
	g.editing.Clear()
	return nil
}

// Init initializes the GUI.
func (g *GUI) Init(caption string, bgImg, symbolFont []byte) error {
	var err error
	if g.window, g.context, err = newWindow(winWidth, winHeight, caption); err != nil {
		return errors.Wrap(err, "gui: failed to create a new window")
	}
	g.window.SetDropCallback(func(w *window, filenames []string) {
		g.DropFiles(filenames)
	})

	g.symbolFont = symbolFont
	g.lastDPI = g.window.DPI()
	if err = g.initFont(symbolFont, g.window.Scale()); err != nil {
		return errors.Wrap(err, "gui: failed to load a font")
	}

	g.tabView = tabview.New()

	g.layerView, err = layerview.New(g.font.MainHandle, g.font.SymbolHandle)
	if err != nil {
		return errors.Wrap(err, "gui: failed to initialize layerview")
	}
	g.layerView.ReportError = g.ReportError
	g.layerView.ExportFaviewSlider = func(path, sliderName string, names, values []string, selectedIndex int) {
		if err := g.ExportFaviewSlider(path, sliderName, names, values, selectedIndex); err != nil {
			g.ReportError(errors.Wrap(err, "gui: cannot export faview slider"))
		}
	}
	g.layerView.ExportLayerNames = func(path string, names, values []string, selectedIndex int) {
		if err := g.ExportLayerNames(path, names, values, selectedIndex); err != nil {
			g.ReportError(errors.Wrap(err, "gui: cannot export layer names"))
		}
	}

	bg, err := png.Decode(bytes.NewReader(bgImg))
	if err != nil {
		return errors.Wrap(err, "gui: could not decode bg.png")
	}
	g.mainView, err = mainview.New(bg)
	if err != nil {
		return errors.Wrap(err, "gui: failed to initialize mainview")
	}
	g.mainView.SetZoomRange(-5, 0, 0.001)
	close(g.ready)
	return nil
}

func (g *GUI) do(f func() error) error {
	done := make(chan error)
	g.queue <- func() {
		defer func() {
			if err := recover(); err != nil {
				ods.Recover(err)
				done <- errors.Errorf("unexpected error occurred: %v", err)
			}
		}()
		done <- f()
	}
	return <-done
}

// Main runs the main GUI loop.
func (g *GUI) Main(exitCh <-chan struct{}) {
	defer func() {
		if err := recover(); err != nil {
			ods.Recover(err)
		}
		g.freeFont()
		nk.NkPlatformShutdown()
		g.terminate()
	}()
	fpsTicker := time.NewTicker(time.Second / 30)
	for {
		select {
		case f := <-g.queue:
			f()

		case <-exitCh:
			fpsTicker.Stop()
			return

		case <-fpsTicker.C:
			g.pollEvents()
			if g.window.ShouldClose() {
				g.window.Hide()
				g.window.SetShouldClose(false)
			}
			g.update()
		}
	}
}

func b2i(b bool) int32 {
	if b {
		return 1
	}
	return 0
}

func (g *GUI) uiScale() float32 {
	if g.window == nil {
		return 1
	}
	s := g.window.Scale()
	if s <= 0 {
		return 1
	}
	return s
}

func (g *GUI) scaledInt(base int) int {
	v := int(float32(base)*g.uiScale() + 0.5)
	if v < 1 {
		v = 1
	}
	return v
}

func (g *GUI) updateFontIfDPIChanged() {
	if g.window == nil {
		return
	}
	dpi := g.window.DPI()
	if dpi == 0 || dpi == g.lastDPI {
		return
	}
	if err := g.initFont(g.symbolFont, g.window.Scale()); err != nil {
		g.ReportError(errors.Wrap(err, "gui: failed to reload font after DPI change"))
		return
	}
	g.lastDPI = dpi
	if g.layerView != nil {
		g.layerView.SetFontHandles(g.font.MainHandle, g.font.SymbolHandle)
		g.layerView.SetScale(g.uiScale())
		if g.img != nil {
			g.layerView.UpdateLayerThumbnails(g.img.PSD, g.scaledInt(24), g.do)
		}
	}
}

func (g *GUI) changeSelectedImage() {
	if g.snapshot.SelectedIndex < 0 || g.snapshot.SelectedIndex >= len(g.snapshot.Items) {
		g.img = nil
		g.mainView.Clear()
		return
	}

	item := g.snapshot.Items[g.snapshot.SelectedIndex]
	g.img = item.Image
	g.mainView.Clear()
	if g.img == nil {
		return
	}

	g.thumbnailer = g.editing.CreateThumbnailer(g.snapshot.SelectedIndex)
	updateRenderedImage(g, g.img)
	g.layerView.SetScale(g.uiScale())
	g.layerView.UpdateLayerThumbnails(g.img.PSD, g.scaledInt(24), g.do)
}

func (g *GUI) update() {
	ctx := g.context
	g.updateFontIfDPIChanged()
	nk.NkPlatformNewFrame()
	width, height := g.window.GetSize()
	scale := g.uiScale()
	p := float32(2) * scale
	sidePaneWidth := float32(360) * scale
	topPaneHeight := float32(28) * scale
	closeButtonWidth := float32(28) * scale
	sideTabPaneWidth := float32(64) * scale
	if g.layerView != nil {
		g.layerView.SetScale(scale)
	}

	modified := false

	nk.NkStylePushVec2(ctx, nkhelper.GetStyleWindowPaddingPtr(ctx), nk.NkVec2(0, 0))
	nk.NkStylePushVec2(ctx, nkhelper.GetStyleWindowGroupPaddingPtr(ctx), nk.NkVec2(0, 0))

	if nk.NkBegin(ctx, "MainWindow", nk.NkRect(0, 0, float32(width), float32(height)), nk.WindowNoScrollbar) != 0 {
		nk.NkLayoutRowBegin(ctx, nk.Static, float32(height)-p, 2)

		nk.NkLayoutRowPush(ctx, sidePaneWidth-p)
		if nk.NkGroupBegin(ctx, "UIPane", nk.WindowNoScrollbar) != 0 {
			if g.img != nil {
				rgn := nk.NkWindowGetContentRegion(ctx)

				if nk.NkInputIsKeyDown(ctx.Input(), nk.KeyCopy) != 0 {
					g.setClipboard()
				}
				nk.NkLayoutRowBegin(ctx, nk.Dynamic, topPaneHeight-p, 4)
				nk.NkLayoutRowPush(ctx, 0.3)
				if nk.NkButtonLabel(ctx, locale.Pgettext("gui", "Send")) != 0 || nkhelper.IsPressed(ctx, 19) { // 19 = ^S
					g.sendEditingImage()
				}
				nk.NkLayoutRowPush(ctx, 0.3)
				if nk.NkButtonLabel(ctx, locale.Pgettext("gui", "Capture")) != 0 {
					modified = g.loadEditingImage() || modified
				}
				nk.NkLayoutRowPush(ctx, 0.2)
				fx, fy := g.img.FlipX(), g.img.FlipY()
				if (nk.NkSelectLabel(ctx, "⇆", nk.TextAlignCentered|nk.TextAlignMiddle, b2i(fx)) != 0) != fx {
					modified = g.img.SetFlipX(!fx) || modified
				}
				if (nk.NkSelectLabel(ctx, "⇅", nk.TextAlignCentered|nk.TextAlignMiddle, b2i(fy)) != 0) != fy {
					modified = g.img.SetFlipY(!fy) || modified
				}
				nk.NkLayoutRowEnd(ctx)

				nk.NkLayoutRowBegin(ctx, nk.Static, rgn.H()-p, 3)

				nk.NkLayoutRowPush(ctx, sideTabPaneWidth-p)
				if nk.NkGroupBegin(ctx, "SideTabPane", nk.WindowNoScrollbar) != 0 {
					thumbnails, _ := g.thumbCache.Update(g.snapshot.Items)
					n0 := g.snapshot.SelectedIndex
					n1 := g.tabView.Render(ctx, g.snapshot, thumbnails, scale)
					if n0 != n1 {
						g.editing.Select(n1)
					}
					nk.NkGroupEnd(ctx)
				}

				nk.NkLayoutRowPush(ctx, rgn.W()-sideTabPaneWidth-p)
				if nk.NkGroupBegin(ctx, "LayerTreePane", nk.WindowNoScrollbar) != 0 {
					modified = g.layerView.Render(ctx, g.img) || modified
					if modified {
						g.img.Modified = true
						g.img.Layers.Normalize()
						updateRenderedImage(g, g.img)
					}
					nk.NkGroupEnd(ctx)
				}
				nk.NkLayoutRowEnd(ctx)
			}

			nk.NkGroupEnd(ctx)
		}

		nk.NkLayoutRowPush(ctx, float32(width)-sidePaneWidth-p)
		if nk.NkGroupBegin(ctx, "MainPane", nk.WindowNoScrollbar) != 0 {
			if g.img != nil {
				rgn := nk.NkWindowGetContentRegion(ctx)

				displayName := ""
				if g.snapshot.SelectedIndex >= 0 && g.snapshot.SelectedIndex < len(g.snapshot.Items) {
					displayName = g.snapshot.Items[g.snapshot.SelectedIndex].DisplayName
				}

				nk.NkLayoutRowBegin(ctx, nk.Static, topPaneHeight-p, 2)
				nk.NkLayoutRowPush(ctx, rgn.W()-closeButtonWidth-p)
				nk.NkLabel(ctx, displayName, nk.TextCentered)

				nk.NkLayoutRowPush(ctx, closeButtonWidth-p)
				if nk.NkButtonLabel(ctx, "×") != 0 {
					g.editing.Delete(g.snapshot.SelectedIndex)
				}

				nk.NkLayoutRowEnd(ctx)

				nk.NkLayoutRowBegin(ctx, nk.Static, rgn.H()-p, 3)

				nk.NkLayoutRowPush(ctx, float32(rgn.W()))
				if nk.NkGroupBegin(ctx, "MainPane", nk.WindowNoScrollbar) != 0 {
					g.mainView.Render(ctx, scale)
					nk.NkGroupEnd(ctx)
				}

				nk.NkLayoutRowEnd(ctx)
			}
			nk.NkGroupEnd(ctx)
		}
		nk.NkLayoutRowEnd(ctx)
	}
	nk.NkEnd(ctx)

	nk.NkStylePopVec2(ctx)
	nk.NkStylePopVec2(ctx)

	g.window.Render()
}

func (g *GUI) sendEditingImage() {
	state, err := g.img.Serialize()
	if err != nil {
		g.ReportError(errors.Wrap(err, "gui: cannot serialize"))
		return
	}
	err = g.SendEditingImageState(*g.img.FilePath, state)
	if err != nil {
		g.ReportError(errors.Wrap(err, "gui: cannot send editing image state"))
	}
}

func (g *GUI) setClipboard() {
	img, err := g.img.Render(context.Background())
	if err != nil {
		g.ReportError(errors.Wrap(err, "gui: failed to render image"))
		return
	}
	err = clipboard.SetImage(img)
	if err != nil {
		g.ReportError(errors.Wrap(err, "gui: failed to set clipboard"))
		return
	}
}

func (g *GUI) loadEditingImage() bool {
	latestState := ""
	if g.snapshot.SelectedIndex >= 0 && g.snapshot.SelectedIndex < len(g.snapshot.Items) {
		latestState = g.snapshot.Items[g.snapshot.SelectedIndex].LatestState
	}
	modified, err := g.img.Deserialize(latestState)
	if err != nil {
		g.ReportError(errors.Wrap(err, "gui: cannot load state"))
		return false
	}
	return modified
}

// ReportError reports an error to the user.
func (g *GUI) ReportError(err error) {
	//TODO: improve error handling
	ods.ODS("error: %v", err)
}

// GetWindowHandle returns the native window handle.
func (g *GUI) GetWindowHandle() (uintptr, error) {
	<-g.ready
	return g.window.NativeWindow(), nil
}

// Serialize serializes the editing state (called from IPC thread).
func (g *GUI) Serialize() (string, error) {
	return g.editing.Serialize()
}

// Deserialize deserializes the editing state (called from IPC thread).
func (g *GUI) Deserialize(state string) error {
	return g.editing.Deserialize(state)
}

// Touch touches all images (called from IPC thread).
func (g *GUI) Touch() {
	g.editing.Touch()
}
