package mainview

import (
	"context"
	"math"

	"psdtoolkit/img"
	"psdtoolkit/ods"
)

type viewResizeMode int

const (
	vrmFast viewResizeMode = iota
	vrmBeautiful
	vrmFastAfterBeautiful
)

func (mv *MainView) updateViewImage(mode viewResizeMode) {
	if mv.currentImg == nil || mv.renderScaled == nil {
		return
	}

	jq.CancelAll()
	jq.Enqueue(func(ctx context.Context) error {
		// Calculate the scale based on zoom level
		// zoom < 0 means downscale (scale < 1)
		// zoom >= 0 means no downscale needed (scale = 1, magnification handled at display)
		var scale float64 = 1.0
		if mv.zoom < 0 {
			scale = math.Pow(2, mv.zoom)
		}

		// Render with fast quality first
		if mode == vrmFast || mode == vrmFastAfterBeautiful {
			resizedImage, err := mv.renderScaled(ctx, mv.currentImg, scale, img.ScaleQualityFast)
			if err != nil || resizedImage == nil {
				ods.ODS("renderScaled(fast): aborted or nil")
				return nil
			}
			mv.do(func() {
				mv.renderedImage = resizedImage
				mv.resizedImage = resizedImage
				mv.forceUpdate = true
			})
			// Notify for thumbnail update (using fast render result)
			if mv.onImageRendered != nil {
				mv.onImageRendered(resizedImage)
			}
		}

		if mode == vrmFast {
			return nil
		}

		// Render with beautiful quality
		resizedImage, err := mv.renderScaled(ctx, mv.currentImg, scale, img.ScaleQualityBeautiful)
		if err != nil || resizedImage == nil {
			ods.ODS("renderScaled(beautiful): aborted or nil")
			return nil
		}
		mv.do(func() {
			mv.renderedImage = resizedImage
			mv.resizedImage = resizedImage
			mv.forceUpdate = true
		})
		return nil
	})
}
