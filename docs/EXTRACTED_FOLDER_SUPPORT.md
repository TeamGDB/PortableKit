# Extracted PSP game folders

The installer can accept either an uncompressed PSP ISO or a folder whose top level contains:

- `PSP_GAME/PARAM.SFO`
- `PSP_GAME/SYSDIR/EBOOT.BIN`

It checks the same `DISC_ID` and executable SHA-256 as an ISO install. On desktop platforms the folder is selected from the setup file browser and is used in place. On Android, the system folder picker copies the selected tree into app-private storage first.

The runtime exposes the extracted files through a file-backed virtual disc so ordinary `disc0:/PSP_GAME/...` file operations can use them. This is an initial compatibility path, not a byte-for-byte ISO image: raw disc-sector reads do not see ISO volume descriptors or real ISO directory records. A game that depends on those raw sectors may still require a real ISO. Folder-based boot must be tested per title before claiming compatibility.
