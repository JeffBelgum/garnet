// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Package index implements a basic index of packages and their relative
// installation states, as well as thier various top level metadata properties.
package index

import (
	"io/ioutil"
	"os"
	"path/filepath"
	"sort"
	"sync"

	"fuchsia.googlesource.com/pm/pkg"
)

// DynamicIndex provides concurrency safe access to a dynamic index of packages and package metadata
type DynamicIndex struct {
	root string

	// mu protects all following fields
	mu sync.Mutex

	// installing is a map of merkleroot -> package name/version
	installing map[string]pkg.Package

	// needs is a map of blob merkleroot -> [package merkleroot] for packages that need blobs
	needs map[string][]string

	// waiting is a map of package merkleroot -> [blob merkleroots]
	waiting map[string][]string
}

// NewDynamic initializes an DynamicIndex with the given root path.
func NewDynamic(root string) *DynamicIndex {
	// TODO(PKG-14): error is deliberately ignored. This should not be fatal to boot.
	_ = os.MkdirAll(root, os.ModePerm)
	return &DynamicIndex{
		root:       root,
		installing: make(map[string]pkg.Package),
		needs:      make(map[string][]string),
		waiting:    make(map[string][]string),
	}
}

// List returns a list of all known packages in byte-lexical order.
func (idx *DynamicIndex) List() ([]pkg.Package, error) {
	paths, err := filepath.Glob(idx.PackageVersionPath("*", "*"))
	if err != nil {
		return nil, err
	}
	sort.Strings(paths)
	pkgs := make([]pkg.Package, len(paths))
	for i, path := range paths {
		pkgs[i].Version = filepath.Base(path)
		pkgs[i].Name = filepath.Base(filepath.Dir(path))
	}
	return pkgs, nil
}

// Add adds a package to the index
func (idx *DynamicIndex) Add(p pkg.Package, root string) error {
	if err := os.MkdirAll(idx.PackagePath(p.Name), os.ModePerm); err != nil {
		return err
	}

	return ioutil.WriteFile(idx.PackagePath(filepath.Join(p.Name, p.Version)), []byte(root), os.ModePerm)
}

func (idx *DynamicIndex) PackagePath(name string) string {
	return filepath.Join(idx.PackagesDir(), name)
}

func (idx *DynamicIndex) PackageVersionPath(name, version string) string {
	return filepath.Join(idx.PackagesDir(), name, version)
}

func (idx *DynamicIndex) PackagesDir() string {
	dir := filepath.Join(idx.root, "packages")
	// TODO(PKG-14): refactor out the initialization logic so that we can do this once, at an appropriate point in the runtime.
	_ = os.MkdirAll(dir, os.ModePerm)
	return dir
}

func (idx *DynamicIndex) AddNeeds(root string, p pkg.Package, blobs []string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	idx.installing[root] = p
	for _, blob := range blobs {
		idx.needs[blob] = append(idx.needs[blob], root)
	}
	idx.waiting[root] = blobs
}

func (idx *DynamicIndex) Fulfill(need string) {
	idx.mu.Lock()
	defer idx.mu.Unlock()
	packageRoots := idx.needs[need]
	delete(idx.needs, need)
	for _, pkgRoot := range packageRoots {
		oldWaiting := idx.waiting[pkgRoot]
		if len(oldWaiting) == 1 {
			delete(idx.waiting, pkgRoot)
			idx.Add(idx.installing[pkgRoot], pkgRoot)
			delete(idx.installing, pkgRoot)
			continue
		}

		newWaiting := make([]string, 0, len(idx.waiting[pkgRoot])-1)
		for i, blob := range oldWaiting {
			if need != blob {
				newWaiting = append(newWaiting, oldWaiting[i])
			}
		}
		idx.waiting[pkgRoot] = newWaiting
	}
}

func (idx *DynamicIndex) HasNeed(root string) bool {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	_, found := idx.needs[root]
	return found
}

func (idx *DynamicIndex) NeedsList() []string {
	idx.mu.Lock()
	defer idx.mu.Unlock()

	names := make([]string, 0, len(idx.needs))
	for name, _ := range idx.needs {
		names = append(names, name)
	}

	return names
}
