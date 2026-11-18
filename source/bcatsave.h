// Direct access to the BCAT delivery cache save: the save bcat itself keeps,
// opened with fs, not through bcat:s.
//
// From bcat and sdb in 23.5.0:
// - /directories/<dir>/files.meta is a u32 version (1), then up to 100
//   0x80-byte entries: name[0x20], u64 data_id @0x20, u64 size @0x28,
//   digest[0x10] @0x30, the rest zero. bcat copies all of it from the server's
//   list when it downloads, and never hashes the file itself.
// - /directories.meta is a u32 version (1), then up to 100 0x40-byte
//   entries: name[0x20], then 0x20 bytes from the server's list (digest).
// - bcat's IDeliveryCacheFileService Open needs the name in files.meta (not
//   directories.meta, which only the directory listing uses), but
//   GetSize and Read use the real file. GetDigest returns the meta digest.
// - sdb's DeliveryCacheFile only does Open, GetSize, Read and Close. It never
//   asks for the digest. It rejects compatibility_version >= 2.
// So the file is written at its own size, the meta size is updated to match
// (only the directory listing reports it), and the digest is left alone.
#pragma once
#include <switch.h>

#define META_ENTRY 0x80      // files.meta
#define DIR_META_ENTRY 0x40  // directories.meta
#define META_MAX 100
#define META_SIZE_OFF 0x28

// Reads a whole file from a save. *data is malloc'd and NUL-terminated.
Result read_save_file(FsFileSystem *fs, const char *p, u8 **data, u64 *len);
// Writes a whole file to SD.
bool write_file(const char *path, const void *data, size_t len);
bool fs_not_found(Result rc);
// Opens the delivery cache save that read_bcat_save found, read/write.
Result open_bcat_save_rw(FsFileSystem *fs);
// Copies every file in the save under src to dst on SD, listing each one.
// Returns the file count, or -1 if anything couldn't be copied.
int backup_tree(FsFileSystem *fs, const char *src, const char *dst);
// Writes blacklist.json and makes sure bcat's meta files list it, in one
// commit. An existing entry keeps its data_id and digest and only gets the
// new size; a missing entry is added with both zero, and the directory is
// added to directories.meta the same way.
Result write_save_blacklist(FsFileSystem *fs, const u8 *data, u64 len);
// Undoes write_save_blacklist on a save that had no blacklist.json: deletes
// the file and its files.meta entry, and the directory too if that leaves
// it empty.
Result remove_save_blacklist(FsFileSystem *fs);
