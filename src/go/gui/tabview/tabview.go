package tabview

import (
	"github.com/golang-ui/nuklear/nk"

	"psdtoolkit/imgmgr/editing"
	"psdtoolkit/ods"
)

// TabView renders the tab pane for selecting images.
type TabView struct{}

// New creates a new TabView.
func New() *TabView {
	return &TabView{}
}

func (v *TabView) drawTab(ctx *nk.Context, img nk.Image, selected bool, scale float32) (clicked bool) {
	var rect nk.Rect
	state := nk.NkWidget(&rect, ctx)
	if state == 0 {
		return false
	}

	canvas := nk.NkWindowGetCanvas(ctx)
	var bg nk.Color

	if selected {
		bg.SetA(64)
	} else {
		if state != nk.WidgetRom {
			if nk.NkWidgetIsHovered(ctx) != 0 {
				bg.SetA(32)
				clicked = nk.NkInputIsMousePressed(ctx.Input(), nk.ButtonLeft) != 0
			}
		}
	}
	nk.NkFillRect(canvas, rect, 0, bg)

	var white nk.Color
	white.SetRGBA(255, 255, 255, 255)
	if scale <= 0 {
		scale = 1
	}
	size := float32(48) * scale
	nk.NkDrawImage(canvas, nk.NkRect(rect.X(), rect.Y(), size, size), &img, white)
	return clicked
}

// Render renders the tab view and returns the new selected index.
func (v *TabView) Render(ctx *nk.Context, snapshot editing.Snapshot, thumbnails []nk.Image, scale float32) int {
	rgn := nk.NkWindowGetContentRegion(ctx)
	winHeight := rgn.H()

	if scale <= 0 {
		scale = 1
	}
	padding := float32(2) * scale
	tabHeight := float32(48) * scale
	r := snapshot.SelectedIndex

	nk.NkLayoutRowDynamic(ctx, winHeight-padding, 1)
	if nk.NkGroupBegin(ctx, "TabPane", 0) != 0 {
		if len(thumbnails) > 0 {
			nk.NkLayoutSpaceBegin(ctx, nk.Static, tabHeight*float32(len(thumbnails)+1), int32(len(thumbnails)+1))

			for i, img := range thumbnails {
				nk.NkLayoutSpacePush(ctx, nk.NkRect(0, float32(i)*tabHeight, tabHeight, tabHeight))
				if v.drawTab(ctx, img, i == snapshot.SelectedIndex, scale) {
					r = i
					ods.ODS("clicked %d", i)
				}
			}
			nk.NkLayoutSpaceEnd(ctx)
		}
		nk.NkGroupEnd(ctx)
	}

	return r
}
