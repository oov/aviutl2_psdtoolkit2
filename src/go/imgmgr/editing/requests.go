package editing

import (
	"image"

	"psdtoolkit/img"
)

// --- Synchronous requests (wait for Reply) ---

// AddFileReq requests to add a file to the editing list.
type AddFileReq struct {
	FilePath string
	Tag      int
	Reply    chan<- AddFileResp
}

// AddFileResp is the response for AddFileReq.
type AddFileResp struct {
	Index int // >= 0 if already exists, -1 if newly added
	Err   error
}

// SerializeReq requests to serialize the editing state.
type SerializeReq struct {
	Reply chan<- SerializeResp
}

// SerializeResp is the response for SerializeReq.
type SerializeResp struct {
	State string
	Err   error
}

// DeserializeReq requests to deserialize and restore the editing state.
type DeserializeReq struct {
	State string
	Reply chan<- DeserializeResp
}

// DeserializeResp is the response for DeserializeReq.
type DeserializeResp struct {
	Err error
}

// UpdateTagStateReq requests to update the state for a specific tag.
type UpdateTagStateReq struct {
	FilePath string
	Tag      int
	State    string
	Reply    chan<- UpdateTagStateResp
}

// UpdateTagStateResp is the response for UpdateTagStateReq.
type UpdateTagStateResp struct {
	NeedRefresh bool
	Err         error
}

// GetSelectedImageReq requests the currently selected image information.
type GetSelectedImageReq struct {
	Reply chan<- GetSelectedImageResp
}

// GetSelectedImageResp is the response for GetSelectedImageReq.
type GetSelectedImageResp struct {
	Image       *img.Image
	DisplayName string
	LatestState string
}

// GetSnapshotReq requests a snapshot of the current editing state.
type GetSnapshotReq struct {
	Reply chan<- Snapshot
}

// --- Asynchronous requests (no Reply needed) ---

// DeleteReq requests to delete an item at the specified index.
type DeleteReq struct {
	Index int
}

// ClearReq requests to clear all items.
type ClearReq struct{}

// TouchReq requests to touch all images (update last access time).
type TouchReq struct{}

// SelectReq requests to change the selected index.
type SelectReq struct {
	Index int
}

// UpdateThumbnailReq requests to update the thumbnail for an item.
type UpdateThumbnailReq struct {
	Index     int
	Thumbnail *image.NRGBA
}

// UpdateViewStateReq requests to update the view state for an item.
type UpdateViewStateReq struct {
	Index     int
	ViewState *img.ViewState
}

// SetSplitterWidthReq requests to set the splitter width.
type SetSplitterWidthReq struct {
	Width float32
}

// GetSplitterWidthReq requests to get the splitter width.
type GetSplitterWidthReq struct {
	Reply chan<- float32
}
