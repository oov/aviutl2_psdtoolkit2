// Package locale provides system locale detection and font selection for Windows.
// It loads translation resources from PSDToolKit.aux2 to get localized strings.
package locale

import (
	"encoding/binary"
	"strings"
	"sync"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Global translator instance
var (
	globalMO   *MO
	globalOnce sync.Once
)

// Gettext translates a message using the global translator.
func Gettext(msgid string) string {
	globalOnce.Do(func() {
		globalMO, _ = LoadMO()
	})
	if globalMO == nil {
		return msgid
	}
	return globalMO.GetText(msgid)
}

// Pgettext translates a message with context using the global translator.
// This is equivalent to pgettext in GNU gettext.
func Pgettext(msgctxt, msgid string) string {
	globalOnce.Do(func() {
		globalMO, _ = LoadMO()
	})
	if globalMO == nil {
		return msgid
	}
	return globalMO.PGetText(msgctxt, msgid)
}

var (
	kernel32                      = windows.NewLazySystemDLL("kernel32.dll")
	procGetThreadPreferredUILangs = kernel32.NewProc("GetThreadPreferredUILanguages")
	procLocaleNameToLCID          = kernel32.NewProc("LocaleNameToLCID")
	procEnumResourceLanguagesW    = kernel32.NewProc("EnumResourceLanguagesW")
	procFindResourceExW           = kernel32.NewProc("FindResourceExW")
	procLoadResource              = kernel32.NewProc("LoadResource")
	procLockResource              = kernel32.NewProc("LockResource")
	procSizeofResource            = kernel32.NewProc("SizeofResource")
)

const (
	MUI_LANGUAGE_NAME        = 0x8
	LOAD_LIBRARY_AS_DATAFILE = 0x2
)

// getPreferredUILanguages returns the user's preferred UI languages as LANGIDs
func getPreferredUILanguages() ([]uint16, error) {
	var numLangs uint32
	var bufSize uint32

	// First call to get buffer size
	ret, _, _ := procGetThreadPreferredUILangs.Call(
		uintptr(MUI_LANGUAGE_NAME),
		uintptr(unsafe.Pointer(&numLangs)),
		0,
		uintptr(unsafe.Pointer(&bufSize)),
	)
	if ret == 0 || bufSize == 0 {
		return nil, nil
	}

	// Allocate buffer and get languages
	buf := make([]uint16, bufSize)
	ret, _, _ = procGetThreadPreferredUILangs.Call(
		uintptr(MUI_LANGUAGE_NAME),
		uintptr(unsafe.Pointer(&numLangs)),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(unsafe.Pointer(&bufSize)),
	)
	if ret == 0 {
		return nil, nil
	}

	// Parse double-null-terminated string list and convert to LANGIDs
	var langIDs []uint16
	start := 0
	for i := 0; i < len(buf); i++ {
		if buf[i] == 0 {
			if i > start {
				langName := windows.UTF16ToString(buf[start:i])
				langID := localeNameToLCID(langName)
				if langID != 0 {
					langIDs = append(langIDs, uint16(langID&0xFFFF))
				}
			}
			start = i + 1
			if i+1 < len(buf) && buf[i+1] == 0 {
				break
			}
		}
	}

	return langIDs, nil
}

func localeNameToLCID(name string) uint32 {
	namePtr, err := windows.UTF16PtrFromString(name)
	if err != nil {
		return 0
	}
	ret, _, _ := procLocaleNameToLCID.Call(
		uintptr(unsafe.Pointer(namePtr)),
		0,
	)
	return uint32(ret)
}

// enumResourceLanguages enumerates available language resources
func enumResourceLanguages(module windows.Handle, resType, resName uintptr) ([]uint16, error) {
	var langs []uint16

	callback := windows.NewCallback(func(hModule, lpType, lpName uintptr, wLanguage uint16, lParam uintptr) uintptr {
		langs = append(langs, wLanguage)
		return 1 // Continue enumeration
	})

	procEnumResourceLanguagesW.Call(
		uintptr(module),
		resType,
		resName,
		callback,
		0,
	)

	return langs, nil
}

// chooseLanguage selects the best matching language from available resources
func chooseLanguage(preferred, available []uint16) uint16 {
	var candidate uint16
	for _, pref := range preferred {
		prefPrimary := pref & 0x3FF // PRIMARYLANGID
		for _, avail := range available {
			if avail == pref {
				return avail // Exact match
			}
			if candidate == 0 && (avail&0x3FF) == prefPrimary {
				candidate = avail // Primary language match
			}
		}
	}
	return candidate
}

// MO holds a loaded .mo translation file
type MO struct {
	module windows.Handle
	data   []byte
}

// LoadMO loads translations from PSDToolKit.aux2 next to the executable.
// Directory structure: Plugin/PSDToolKit/PSDToolKit.aux2 and Plugin/PSDToolKit/PSDToolKit.exe
func LoadMO() (*MO, error) {
	// Get path to PSDToolKit.aux2
	var exePath [windows.MAX_PATH]uint16
	n, err := windows.GetModuleFileName(0, &exePath[0], windows.MAX_PATH)
	if err != nil || n == 0 {
		return nil, nil
	}

	path := windows.UTF16ToString(exePath[:n])
	lastSep := strings.LastIndexByte(path, '\\')
	if lastSep < 0 {
		return nil, nil
	}
	exeDir := path[:lastSep+1] // Plugin/PSDToolKit/
	aux2Path := exeDir + "PSDToolKit.aux2"

	// Load aux2 as data file (doesn't execute DllMain)
	module, err := windows.LoadLibraryEx(aux2Path, 0, LOAD_LIBRARY_AS_DATAFILE)
	if err != nil {
		return nil, nil
	}

	// Get preferred languages
	preferred, err := getPreferredUILanguages()
	if err != nil || len(preferred) == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Resource type 10 (custom), name "MO"
	resName, _ := windows.UTF16PtrFromString("MO")
	resType := uintptr(10)

	// Enumerate available languages
	available, err := enumResourceLanguages(module, resType, uintptr(unsafe.Pointer(resName)))
	if err != nil || len(available) == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Choose best language
	chosen := chooseLanguage(preferred, available)
	if chosen == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Find resource
	resNamePtr, _ := windows.UTF16PtrFromString("MO")
	hrsrc, _, _ := procFindResourceExW.Call(
		uintptr(module),
		resType,
		uintptr(unsafe.Pointer(resNamePtr)),
		uintptr(chosen),
	)
	if hrsrc == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Load resource
	hglob, _, _ := procLoadResource.Call(uintptr(module), hrsrc)
	if hglob == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Get resource size
	size, _, _ := procSizeofResource.Call(uintptr(module), hrsrc)
	if size < 28 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Lock resource to get pointer
	ptr, _, _ := procLockResource.Call(hglob)
	if ptr == 0 {
		windows.FreeLibrary(module)
		return nil, nil
	}

	// Create byte slice from resource data
	data := unsafe.Slice((*byte)(unsafe.Pointer(ptr)), size)

	return &MO{
		module: module,
		data:   data,
	}, nil
}

// Close releases resources
func (m *MO) Close() {
	if m != nil && m.module != 0 {
		windows.FreeLibrary(m.module)
		m.module = 0
	}
}

// GetText looks up a translation
func (m *MO) GetText(msgid string) string {
	if m == nil || len(m.data) < 28 {
		return msgid
	}

	data := m.data

	// Check magic number and determine endianness
	magic := binary.LittleEndian.Uint32(data[0:4])
	var readU32 func([]byte) uint32
	if magic == 0x950412de {
		readU32 = binary.LittleEndian.Uint32
	} else if magic == 0xde120495 {
		readU32 = binary.BigEndian.Uint32
	} else {
		return msgid
	}

	// Parse header
	version := readU32(data[4:8])
	if version != 0 {
		return msgid
	}

	numStrings := readU32(data[8:12])
	origOffset := readU32(data[12:16])
	transOffset := readU32(data[16:20])

	if int(origOffset) >= len(data) || int(transOffset) >= len(data) {
		return msgid
	}

	// Binary search for msgid
	lo, hi := uint32(0), numStrings
	for lo < hi {
		mid := lo + (hi-lo)/2

		oLen := readU32(data[origOffset+mid*8:])
		oOff := readU32(data[origOffset+mid*8+4:])
		if int(oOff+oLen) > len(data) {
			return msgid
		}

		orig := string(data[oOff : oOff+oLen])
		cmp := strings.Compare(orig, msgid)

		if cmp == 0 {
			// Found! Get translation
			tOff := readU32(data[transOffset+mid*8+4:])
			tLen := readU32(data[transOffset+mid*8:])
			if int(tOff+tLen) > len(data) {
				return msgid
			}
			return string(data[tOff : tOff+tLen])
		} else if cmp < 0 {
			lo = mid + 1
		} else {
			hi = mid
		}
	}

	return msgid
}

// PGetText looks up a translation with context (pgettext equivalent).
// In .mo files, context messages are stored as "context\x04msgid".
func (m *MO) PGetText(msgctxt, msgid string) string {
	// GNU gettext stores context messages as "context\x04msgid"
	key := msgctxt + "\x04" + msgid
	result := m.GetText(key)
	if result == key {
		// No translation found, return original msgid
		return msgid
	}
	return result
}

// FontList contains font names to try in order of preference
type FontList struct {
	UIFonts []string
}

// defaultFontList is used when no translation is available
var defaultFontList = FontList{
	UIFonts: []string{
		"Segoe UI",
		"Tahoma",
		"Arial",
	},
}

// GetPreferredFontList returns the font list based on the loaded translations
func GetPreferredFontList() FontList {
	mo, err := LoadMO()
	if err != nil || mo == nil {
		return defaultFontList
	}
	defer mo.Close()

	fontListStr := mo.GetText("dialog_ui_font")
	if fontListStr == "dialog_ui_font" {
		// No translation found
		return defaultFontList
	}

	// Parse newline-separated font list
	lines := strings.Split(fontListStr, "\n")
	var fonts []string
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line != "" {
			fonts = append(fonts, line)
		}
	}

	if len(fonts) == 0 {
		return defaultFontList
	}

	return FontList{UIFonts: fonts}
}
