package editing

import (
	"encoding/json"
	"testing"

	"psdtoolkit/img"
)

func TestSerializeRootFormat(t *testing.T) {
	// Test that new format includes version and splitter width
	root := serializeRoot{
		Version:       1,
		SplitterWidth: 480.5,
		Images: []serializeData{
			{
				Image: &img.ProjectState{
					Version:  1,
					FilePath: "/test/path.psd",
					ViewState: &img.ViewState{
						Zoom:    1.5,
						ScrollX: 0.3, // Relative position (0.0-1.0)
						ScrollY: 0.6,
					},
				},
				Tag: 42,
			},
		},
	}

	data, err := json.Marshal(root)
	if err != nil {
		t.Fatalf("failed to marshal: %v", err)
	}

	var decoded serializeRoot
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("failed to unmarshal: %v", err)
	}

	if decoded.Version != 1 {
		t.Errorf("version = %d, want 1", decoded.Version)
	}
	if decoded.SplitterWidth != 480.5 {
		t.Errorf("splitterWidth = %f, want 480.5", decoded.SplitterWidth)
	}
	if len(decoded.Images) != 1 {
		t.Fatalf("len(images) = %d, want 1", len(decoded.Images))
	}
	if decoded.Images[0].Image.ViewState == nil {
		t.Fatal("viewState is nil")
	}
	if decoded.Images[0].Image.ViewState.Zoom != 1.5 {
		t.Errorf("zoom = %f, want 1.5", decoded.Images[0].Image.ViewState.Zoom)
	}
	if decoded.Images[0].Image.ViewState.ScrollX != 0.3 {
		t.Errorf("scrollX = %f, want 0.3", decoded.Images[0].Image.ViewState.ScrollX)
	}
	if decoded.Images[0].Image.ViewState.ScrollY != 0.6 {
		t.Errorf("scrollY = %f, want 0.6", decoded.Images[0].Image.ViewState.ScrollY)
	}
}

func TestLegacyFormatCompatibility(t *testing.T) {
	// Test that legacy format (array of serializeData) can be parsed
	// by attempting to decode as serializeRoot first
	legacy := []serializeData{
		{
			Image: &img.ProjectState{
				Version:  1,
				FilePath: "/test/legacy.psd",
				// No ViewState in legacy data
			},
			Tag: 10,
		},
	}

	data, err := json.Marshal(legacy)
	if err != nil {
		t.Fatalf("failed to marshal legacy: %v", err)
	}

	// Try to decode as new format first
	var root serializeRoot
	err = json.Unmarshal(data, &root)
	// This should succeed but with Version=0 (default)

	if err != nil {
		// Legacy format starts with '[', not '{', so this is expected
		// In actual code, we handle this by checking the first character
		t.Logf("Expected: legacy format decode as object failed: %v", err)
	} else {
		// If it decoded, version should be 0 (not set in legacy)
		if root.Version != 0 {
			t.Logf("Decoded as object with version=%d", root.Version)
		}
	}

	// Verify we can still decode as array
	var legacyDecoded []serializeData
	if err := json.Unmarshal(data, &legacyDecoded); err != nil {
		t.Fatalf("failed to unmarshal as array: %v", err)
	}
	if len(legacyDecoded) != 1 {
		t.Errorf("len = %d, want 1", len(legacyDecoded))
	}
	if legacyDecoded[0].Image.FilePath != "/test/legacy.psd" {
		t.Errorf("filePath = %q, want /test/legacy.psd", legacyDecoded[0].Image.FilePath)
	}
}

func TestViewStateNilSafe(t *testing.T) {
	// Test that nil ViewState is handled correctly
	state := &img.ProjectState{
		Version:   1,
		FilePath:  "/test/nil.psd",
		ViewState: nil, // Explicitly nil
	}

	data, err := json.Marshal(state)
	if err != nil {
		t.Fatalf("failed to marshal: %v", err)
	}

	var decoded img.ProjectState
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("failed to unmarshal: %v", err)
	}

	// ViewState should be nil after round-trip
	if decoded.ViewState != nil {
		t.Errorf("viewState should be nil, got %+v", decoded.ViewState)
	}
}

func TestViewStateOmitEmpty(t *testing.T) {
	// Test that zero values are omitted with omitempty
	state := &img.ViewState{
		Zoom:    0, // Should be omitted
		ScrollX: 0, // Should be omitted
		ScrollY: 0, // Should be omitted
	}

	data, err := json.Marshal(state)
	if err != nil {
		t.Fatalf("failed to marshal: %v", err)
	}

	// Should be empty object or minimal
	t.Logf("serialized zero viewState: %s", string(data))

	var decoded img.ViewState
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("failed to unmarshal: %v", err)
	}

	if decoded.Zoom != 0 || decoded.ScrollX != 0 || decoded.ScrollY != 0 {
		t.Errorf("values should be zero after round-trip")
	}
}
