// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package pkgfs

import (
	"bytes"
	"encoding/json"
	"io"
	"io/ioutil"
	"log"
	"os"
	"regexp"
	"strings"
	"time"

	"thinfs/fs"

	"fuchsia.googlesource.com/far"
	"fuchsia.googlesource.com/pm/pkg"
)

const (
	identityBlob = "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b"
)

var merklePat = regexp.MustCompile("^[0-9a-f]{64}$")

/*
The following VNodes are defined in this file:

/install           - installDir
/install/pkg       - installPkgDir
/install/pkg/{f}   - installFile{isPkg:true}
/install/blob      - installBlobDir
/install/blob/{f}  - installFile{isPkg:false}
*/

// installDir is located at /install.
type installDir struct {
	unsupportedDirectory

	fs *Filesystem
}

func (d *installDir) Dup() (fs.Directory, error) {
	return d, nil
}

func (d *installDir) Stat() (int64, time.Time, time.Time, error) {
	return 0, d.fs.mountTime, d.fs.mountTime, nil
}

func (d *installDir) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)
	if name == "" {
		return nil, d, nil, nil
	}

	parts := strings.SplitN(name, "/", 2)

	var nd fs.Directory
	switch parts[0] {
	case "pkg":
		nd = &installPkgDir{fs: d.fs}
	case "blob":
		nd = &installBlobDir{fs: d.fs}
	default:
		return nil, nil, nil, fs.ErrNotFound
	}

	if len(parts) == 1 {
		if flags.Directory() {
			return nil, nd, nil, nil
		}
		return nil, nd, nil, fs.ErrNotSupported
	}

	return nd.Open(parts[1], flags)
}

func (d *installDir) Read() ([]fs.Dirent, error) {
	return []fs.Dirent{dirDirEnt("pkg"), dirDirEnt("blob")}, nil
}

func (d *installDir) Close() error {
	return nil
}

// installPkgDir is located at /install/pkg
type installPkgDir struct {
	unsupportedDirectory

	fs *Filesystem
}

func (d *installPkgDir) Dup() (fs.Directory, error) {
	return d, nil
}

func (d *installPkgDir) Stat() (int64, time.Time, time.Time, error) {
	return 0, d.fs.mountTime, d.fs.mountTime, nil
}

func (d *installPkgDir) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)
	if name == "" {
		return nil, d, nil, nil
	}

	if !flags.Create() {
		return nil, nil, nil, fs.ErrNotSupported
	}

	f := &installFile{fs: d.fs, name: name, isPkg: true}
	err := f.open()
	return f, nil, nil, err
}

func (d *installPkgDir) Read() ([]fs.Dirent, error) {
	return []fs.Dirent{}, nil
}

func (d *installPkgDir) Close() error {
	return nil
}

// installBlobDir is located at /install/blob
type installBlobDir struct {
	unsupportedDirectory

	fs *Filesystem
}

func (d *installBlobDir) Dup() (fs.Directory, error) {
	return d, nil
}

func (d *installBlobDir) Stat() (int64, time.Time, time.Time, error) {
	return 0, d.fs.mountTime, d.fs.mountTime, nil
}

func (d *installBlobDir) Open(name string, flags fs.OpenFlags) (fs.File, fs.Directory, *fs.Remote, error) {
	name = clean(name)
	if name == "" {
		return nil, d, nil, nil
	}

	// TODO(raggi): support write resumption..

	if !flags.Create() {
		return nil, nil, nil, fs.ErrNotSupported
	}

	f := &installFile{fs: d.fs, name: name, isPkg: false}
	err := f.open()
	return f, nil, nil, err
}

func (d *installBlobDir) Read() ([]fs.Dirent, error) {
	return []fs.Dirent{}, nil
}

func (d *installBlobDir) Close() error {
	return nil
}

type installFile struct {
	unsupportedFile
	fs *Filesystem

	isPkg bool

	size    uint64
	written uint64
	name    string

	blob *os.File
}

func (f *installFile) open() error {
	if !merklePat.Match([]byte(f.name)) {
		return fs.ErrInvalidArgs
	}

	if f.fs.blobfs.HasBlob(f.name) {
		return fs.ErrAlreadyExists
	}

	var err error
	f.blob, err = os.Create(f.fs.blobfs.PathOf(f.name))
	if err != nil {
		return goErrToFSErr(err)
	}
	return nil
}

func (f *installFile) Write(p []byte, off int64, whence int) (int, error) {
	if whence != fs.WhenceFromCurrent || off != 0 {
		return 0, fs.ErrNotSupported
	}

	n, err := f.blob.Write(p)

	f.written += uint64(n)

	if f.written >= f.size && err == nil {
		f.fs.index.Fulfill(f.name)

		if f.isPkg {
			// TODO(raggi): use already open file instead of re-opening the file
			importPackage(f.fs, f.name)
		}
	}

	return n, goErrToFSErr(err)
}

func (f *installFile) Close() error {
	return goErrToFSErr(f.blob.Close())
}

func (f *installFile) Stat() (int64, time.Time, time.Time, error) {
	return int64(f.written), time.Time{}, time.Time{}, nil
}

func (f *installFile) Truncate(sz uint64) error {
	f.size = sz
	err := f.blob.Truncate(int64(f.size))

	if f.size == 0 && f.name == identityBlob && err == nil {
		f.fs.index.Fulfill(f.name)
	}

	return goErrToFSErr(err)
}

// importPackage reads a package far from blobfs, given a content key, and imports it into the package index
func importPackage(fs *Filesystem, root string) {
	log.Printf("pkgfs: importing package from %q", root)

	f, err := fs.blobfs.Open(root)
	if err != nil {
		log.Printf("error importing package: %s", err)
		return
	}
	defer f.Close()

	// TODO(raggi): this is a bit messy, the system could instead force people to
	// write to specific paths in the incoming directory
	if !far.IsFAR(f) {
		log.Printf("pkgfs:importPackage: %q is not a package, ignoring import", root)
		return
	}
	f.Seek(0, io.SeekStart)

	r, err := far.NewReader(f)
	if err != nil {
		log.Printf("error reading package archive package: %s", err)
		return
	}

	// TODO(raggi): this can also be replaced if we enforce writes into specific places in the incoming tree
	var isPkg bool
	for _, f := range r.List() {
		if strings.HasPrefix(f, "meta/") {
			isPkg = true
		}
	}
	if !isPkg {
		log.Printf("pkgfs: %q does not contain a meta directory, assuming it is not a package", root)
		return
	}

	pf, err := r.ReadFile("meta/package")
	if err != nil {
		log.Printf("error reading package metadata: %s", err)
		return
	}

	var p pkg.Package
	err = json.Unmarshal(pf, &p)
	if err != nil {
		log.Printf("error parsing package metadata: %s", err)
		return
	}

	if err := p.Validate(); err != nil {
		log.Printf("pkgfs: package is invalid: %s", err)
		return
	}

	contents, err := r.ReadFile("meta/contents")
	if err != nil {
		log.Printf("pkgfs: error parsing package contents file for %s: %s", p, err)
		return
	}

	files := bytes.Split(contents, []byte{'\n'})

	// Note: the following heuristic is coarse and not easy to compute. For small
	// packages enumerating all blobs in blobfs will be slow, but for very large
	// packages it's more likely that polling blobfs the expensive way for missing
	// blobs is going to be expensive. This can be improved by blobfs providing an
	// API to handle this case of "which of the following blobs are already
	// readable"
	mayHaveBlob := func(root string) bool { return true }
	if len(files) > 20 {
		infos, err := ioutil.ReadDir(fs.blobfs.Root)
		if err != nil {
			log.Printf("pkgfs: error readdir(%q): %s", fs.blobfs.Root, err)
			return
		}
		names := map[string]struct{}{}
		for _, info := range infos {
			names[info.Name()] = struct{}{}
		}
		mayHaveBlob = func(root string) bool {
			_, found := names[root]
			return found
		}
	}

	needBlobs := map[string]struct{}{}
	for i := range files {
		parts := bytes.SplitN(files[i], []byte{'='}, 2)
		if len(parts) != 2 {
			// TODO(raggi): log illegal contents format?
			continue
		}
		root := string(parts[1])

		// XXX(raggi): this can race, which can deadlock package installs
		if mayHaveBlob(root) && fs.blobfs.HasBlob(root) {
			log.Printf("pkgfs: blob already present for %s: %q", p, root)
			continue
		}

		needBlobs[root] = struct{}{}
	}

	if len(needBlobs) == 0 {
		fs.index.Add(p, root)
	} else {
		fs.index.AddNeeds(root, p, needBlobs)
	}

	go func() {
		log.Printf("pkgfs: asking amber to fetch %d blobs for %s", len(needBlobs), p)
		for root := range needBlobs {
			// TODO(jmatt) limit concurrency, send this to a worker routine?
			fs.amberPxy.GetBlob(root)
		}
	}()
}
