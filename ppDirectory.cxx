// Filename: ppDirectory.cxx
// Created by:  drose (28Sep00)
// 
////////////////////////////////////////////////////////////////////

#include "ppDirectory.h"
#include "ppDirectoryTree.h"
#include "ppScope.h"
#include "ppNamedScopes.h"
#include "ppCommandFile.h"
#include "ppDependableFile.h"
#include "tokenize.h"

#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>

PPDirectory *current_output_directory = (PPDirectory *)NULL;

// An STL object to sort directories in order by dependency and then
// by name, used in get_child_dirnames().
class SortDirectoriesByDependencyAndName {
public:
  bool operator () (const PPDirectory *a, const PPDirectory *b) const {
    if (a->get_depends_index() != b->get_depends_index()) {
      return a->get_depends_index() < b->get_depends_index();
    }
    return a->get_dirname() < b->get_dirname();
  }
};

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::Constructor
//       Access: Public
//  Description: Creates the root directory.
////////////////////////////////////////////////////////////////////
PPDirectory::
PPDirectory(PPDirectoryTree *tree) {
  _scope = (PPScope *)NULL;
  _source = (PPCommandFile *)NULL;
  _parent = (PPDirectory *)NULL;
  _tree = tree;
  _depth = 0;
  _depends_index = 0;
  _computing_depends_index = false;

  _dirname = "top";
  _tree->_dirnames.insert(PPDirectoryTree::Dirnames::value_type(_dirname, this));
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::Constructor
//       Access: Public
//  Description: Creates a new directory level that automatically adds
//               itself to its parent's children list.
////////////////////////////////////////////////////////////////////
PPDirectory::
PPDirectory(const string &dirname, PPDirectory *parent) :
  _dirname(dirname),
  _parent(parent)
{
  assert(_parent != (PPDirectory *)NULL);
  _scope = (PPScope *)NULL;
  _source = (PPCommandFile *)NULL;
  _parent->_children.push_back(this);
  _tree = _parent->_tree;
  _depth = _parent->_depth + 1;
  _depends_index = 0;
  _computing_depends_index = false;

  bool inserted = 
    _tree->_dirnames.insert(PPDirectoryTree::Dirnames::value_type(_dirname, this)).second;
  if (!inserted) {
    cerr << "Warning: multiple directories encountered named "
	 << _dirname << "\n";
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::Destructor
//       Access: Public
//  Description: When a tree root destructs, all of its children are
//               also destroyed.
////////////////////////////////////////////////////////////////////
PPDirectory::
~PPDirectory() {
  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    delete (*ci);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_tree
//       Access: Public
//  Description: Returns the PPDirectoryTree object corresponding to
//               the source tree that this directory is a part of.
////////////////////////////////////////////////////////////////////
PPDirectoryTree *PPDirectory::
get_tree() const {
  return _tree;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::count_source_files
//       Access: Public
//  Description: Returns the number of directories within the tree
//               that actually have a Sources.pp file that was read.
////////////////////////////////////////////////////////////////////
int PPDirectory::
count_source_files() const {
  int count = 0;
  if (_source != (PPCommandFile *)NULL) {
    count++;
  }

  Children::const_iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    count += (*ci)->count_source_files();
  }

  return count;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_dirname
//       Access: Public
//  Description: Returns the name of this particular directory level.
////////////////////////////////////////////////////////////////////
const string &PPDirectory::
get_dirname() const {
  return _dirname;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_depends_index
//       Access: Public
//  Description: Returns the dependency index associated with this
//               directory.  It is generally true that if directory A
//               depends on B, then A.get_depends_index() >
//               B.get_depends_index().
////////////////////////////////////////////////////////////////////
int PPDirectory::
get_depends_index() const {
  return _depends_index;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_path
//       Access: Public
//  Description: Returns the relative path from the root to this
//               particular directory.  This does not include the root
//               name itself, and does not include a trailing slash.
////////////////////////////////////////////////////////////////////
string PPDirectory::
get_path() const {
  if (_parent == (PPDirectory *)NULL) {
    return ".";
  }
  if (_parent->_parent == (PPDirectory *)NULL) {
    return _dirname;
  }
  return _parent->get_path() + "/" + _dirname;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_rel_to
//       Access: Public
//  Description: Returns the relative path to the other directory from
//               this one.  This does not include a trailing slash.
////////////////////////////////////////////////////////////////////
string PPDirectory::
get_rel_to(const PPDirectory *other) const {
  const PPDirectory *a = this;
  const PPDirectory *b = other;

  if (a == b) {
    return ".";
  }

  string prefix, postfix;
  while (a->_depth > b->_depth) {
    prefix += "../";
    a = a->_parent;
    assert(a != (PPDirectory *)NULL);
  }

  while (b->_depth > a->_depth) {
    postfix = b->_dirname + "/" + postfix;
    b = b->_parent;
    assert(b != (PPDirectory *)NULL);
  }

  while (a != b) {
    prefix += "../";
    postfix = b->_dirname + "/" + postfix;
    a = a->_parent;
    b = b->_parent;
    assert(a != (PPDirectory *)NULL);
    assert(b != (PPDirectory *)NULL);
  }

  string result = prefix + postfix;
  assert(!result.empty());
  return result.substr(0, result.length() - 1);
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_source
//       Access: Public
//  Description: Returns the source file associated with this level
//               of the directory hierarchy.  This *might* be NULL.
////////////////////////////////////////////////////////////////////
PPCommandFile *PPDirectory::
get_source() const {
  return _source;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_num_children
//       Access: Public
//  Description: Returns the number of subdirectories below this
//               level.
////////////////////////////////////////////////////////////////////
int PPDirectory::
get_num_children() const {
  return _children.size();
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_child
//       Access: Public
//  Description: Returns the nth subdirectory below this level.
////////////////////////////////////////////////////////////////////
PPDirectory *PPDirectory::
get_child(int n) const {
  assert(n >= 0 && n < (int)_children.size());
  return _children[n];
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_child_dirnames
//       Access: Public
//  Description: Returns a single string listing the names of all the
//               subdirectories of this level, delimited by spaces.
//
//               The list is sorted in dependency order such that a
//               directory is listed after the other directories it
//               might depend on.
////////////////////////////////////////////////////////////////////
string PPDirectory::
get_child_dirnames() const {
  Children copy_children = _children;
  sort(copy_children.begin(), copy_children.end(),
       SortDirectoriesByDependencyAndName());

  vector<string> words;
  Children::const_iterator ci;
  for (ci = copy_children.begin(); ci != copy_children.end(); ++ci) {
    words.push_back((*ci)->get_dirname());
  }

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_complete_subtree
//       Access: Public
//  Description: Returns a single string listing the relative path
//               from the source root to each source directory at this
//               level and below, delimited by spaces.
////////////////////////////////////////////////////////////////////
string PPDirectory::
get_complete_subtree() const {
  Children copy_children = _children;
  sort(copy_children.begin(), copy_children.end(),
       SortDirectoriesByDependencyAndName());

  vector<string> words;
  words.push_back(get_path());

  Children::const_iterator ci;
  for (ci = copy_children.begin(); ci != copy_children.end(); ++ci) {
    words.push_back((*ci)->get_complete_subtree());
  }

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_dependable_file
//       Access: Public
//  Description: Returns a PPDependableFile object corresponding to
//               the named filename, creating one if it does not
//               already exist.  This can be used to determine the
//               inter-file dependencies between source files.
//
//               If is_header is true, then the file will be added to
//               the index at the top of the directory tree, so that
//               other directories may include this file.  In this
//               case, if the filename is not unique, a warning
//               message will be issued.
////////////////////////////////////////////////////////////////////
PPDependableFile *PPDirectory::
get_dependable_file(const string &filename, bool is_header) {
  Dependables::iterator di;
  di = _dependables.find(filename);
  if (di != _dependables.end()) {
    return (*di).second;
  }

  // No such file found; create a new definition.
  PPDependableFile *dependable = new PPDependableFile(this, filename);
  _dependables.insert(Dependables::value_type(filename, dependable));

  if (is_header) {
    bool unique = _tree->_dependables.insert
      (PPDirectoryTree::Dependables::value_type(filename, dependable)).second;
    
    if (!unique) {
      cerr << "Warning: source file " << dependable->get_pathname()
	   << " may be confused with "
	   << _tree->find_dependable_file(filename)->get_pathname()
	   << ".\n";
    }
  }

  return dependable;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::report_depends
//       Access: Public
//  Description: Reports all the directories that the current
//               directory depends on.
////////////////////////////////////////////////////////////////////
void PPDirectory::
report_depends() const {
  if (_i_depend_on.empty()) {
    cerr << _dirname << " depends on no other directories.\n";

  } else {
    // Get the complete set of directories we depend on.
    Depends dep;
    get_complete_i_depend_on(dep);
    
    cerr << _dirname << " depends directly on the following directories:";
    show_directories(_i_depend_on);

    cerr << "and directly or indirectly on the following directories:";
    show_directories(dep);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::report_needs
//       Access: Public
//  Description: Reports all the directories that depend on (need) the
//               current directory.
////////////////////////////////////////////////////////////////////
void PPDirectory::
report_needs() const {
  if (_depends_on_me.empty()) {
    cerr << _dirname << " is needed by no other directories.\n";

  } else {
    // Get the complete set of directories we depend on.
    Depends dep;
    get_complete_depends_on_me(dep);

    cerr << _dirname << " is needed directly by the following directories:";
    show_directories(_depends_on_me);

    cerr << "and directly or indirectly by the following directories:";
    show_directories(dep);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::r_scan
//       Access: Private
//  Description: The recursive implementation of scan_source().
////////////////////////////////////////////////////////////////////
bool PPDirectory::
r_scan(const string &prefix) {
  string root_name = ".";
  if (!prefix.empty()) {
    root_name = prefix.substr(0, prefix.length() - 1);
  }
  
  DIR *root = opendir(root_name.c_str());
  if (root == (DIR *)NULL) {
    cerr << "Unable to scan directory " << root_name << "\n";
    return false;
  }

  struct dirent *d;
  d = readdir(root);
  while (d != (struct dirent *)NULL) {
    string filename = d->d_name;

    if (!filename.empty() && filename[0] != '.') {
      // Is this possibly a subdirectory with its own Sources.pp
      // within it?
      string next_prefix = prefix + filename + "/";
      string source_filename = next_prefix + SOURCE_FILENAME;
      if (access(source_filename.c_str(), F_OK) == 0) {
	PPDirectory *subtree = new PPDirectory(filename, this);

	if (!subtree->r_scan(next_prefix)) {
	  return false;
	}
      }
    }

    d = readdir(root);
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::read_source_file
//       Access: Private
//  Description: Recursively reads in the source file at each level,
//               if defined.
////////////////////////////////////////////////////////////////////
bool PPDirectory::
read_source_file(const string &prefix, PPNamedScopes *named_scopes) {
  string source_filename = prefix + SOURCE_FILENAME;

  ifstream in(source_filename.c_str());
  if (in) {
    //    cerr << "Reading " << source_filename << "\n";

    named_scopes->set_current(_dirname);
    _scope = named_scopes->make_scope("");
    
    _scope->define_variable("SOURCEFILE", SOURCE_FILENAME);
    _scope->define_variable("DIRNAME", _dirname);
    _scope->define_variable("DIRPREFIX", prefix);
    _scope->define_variable("PATH", get_path());
    _scope->define_variable("SUBDIRS", get_child_dirnames());
    _scope->define_variable("SUBTREE", get_complete_subtree());
    _scope->set_directory(this);
    
    _source = new PPCommandFile(_scope);
    
    if (!_source->read_stream(in)) {
      cerr << "Error when reading " << source_filename << "\n";
      return false;
    }
  }
    
  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    if (!(*ci)->read_source_file(prefix + (*ci)->get_dirname() + "/",
				  named_scopes)) {
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::read_depends_file
//       Access: Private
//  Description: Recursively reads in the dependency definition file
//               for each source file.
////////////////////////////////////////////////////////////////////
bool PPDirectory::
read_depends_file(PPNamedScopes *named_scopes) {
  if (_scope != (PPScope *)NULL) {
    // Read the depends file, so we can determine the relationship
    // between this source file and all of the other source files.

    string depends_filename = _scope->expand_variable("DEPENDS_FILE");
    if (depends_filename.empty()) {
      cerr << "No definition given for $[DEPENDS_FILE], cannot process.\n";
      return false;
    }
    
    named_scopes->set_current(_dirname);
    PPCommandFile depends(_scope);
    if (!depends.read_file(depends_filename)) {
      cerr << "Error reading dependency definition file "
	   << depends_filename << ".\n";
      return false;
    }
    
    // This should have defined the variable DEPEND_DIRS, which lists
    // the various dirnames this source file depends on.

    vector<string> dirnames;
    tokenize_whitespace(_scope->expand_variable("DEPEND_DIRS"), dirnames);

    vector<string>::const_iterator ni;
    for (ni = dirnames.begin(); ni != dirnames.end(); ++ni) {
      const string &dirname = (*ni);
      PPDirectory *dir = _tree->find_dirname(dirname);
      if (dir == (PPDirectory *)NULL) {
	cerr << "Could not find dependent dirname " << dirname << "\n";
      } else {
	if (dir != this) {
	  _i_depend_on.insert(dir);
	  dir->_depends_on_me.insert(this);
	}
      }
    }

    // This may also have defined the variable DEPENDABLE_HEADERS,
    // which lists the header files in this directory that C/C++
    // source files in this and other directories might be including
    // (and will therefore depend on).
    vector<string> headers;
    tokenize_whitespace(_scope->expand_variable("DEPENDABLE_HEADERS"), headers);
    for (ni = headers.begin(); ni != headers.end(); ++ni) {
      get_dependable_file(*ni, true);
    }
  }
    
  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    if (!(*ci)->read_depends_file(named_scopes)) {
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::resolve_dependencies
//       Access: Private
//  Description: Visits each directory and assigns a correct
//               _depends_index to each one, such that if directory A
//               depends on directory B then A._depends_index >
//               B._depends_index.
//
//               This also detects cycles in the directory dependency
//               graph.
////////////////////////////////////////////////////////////////////
bool PPDirectory::
resolve_dependencies() {
  if (!compute_depends_index()) {
    return false;
  }

  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    if (!(*ci)->resolve_dependencies()) {
      return false;
    }
  }

  // Now that we've resolved all of our children's dependencies,
  // redefine our SUBDIRS and SUBTREE variables to put things in the
  // right order.
  if (_scope != (PPScope *)NULL) {
    _scope->define_variable("SUBDIRS", get_child_dirnames());
    _scope->define_variable("SUBTREE", get_complete_subtree());
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::compute_depends_index
//       Access: Private
//  Description: Computes the dependency score for a particular
//               directory.  See resolve_dependencies().
////////////////////////////////////////////////////////////////////
bool PPDirectory::
compute_depends_index() {
  if (_depends_index != 0) {
    return true;
  }

  if (_i_depend_on.empty()) {
    _depends_index = 1;
    return true;
  }

  _computing_depends_index = true;
  int max_index = 0;

  Depends::iterator di;
  for (di = _i_depend_on.begin(); di != _i_depend_on.end(); ++di) {
    if ((*di)->_computing_depends_index) {
      // Oops, we have a cycle!
      cerr << "Cycle detected in inter-directory dependencies!\n"
	   << _dirname << " depends on " << (*di)->_dirname << "\n";
      return false;
    }
      
    if (!(*di)->compute_depends_index()) {
      // Keep reporting the cycle as we unroll the recursion.
      cerr << _dirname << " depends on " << (*di)->_dirname << "\n";
      return false;
    }

    max_index = max(max_index, (*di)->_depends_index);
  }

  _computing_depends_index = false;
  _depends_index = max_index + 1;

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::read_file_dependencies
//       Access: Private
//  Description: Before processing the source files, makes a pass and
//               reads in all of the dependency cache files so we'll
//               have a heads-up on which files depend on the others.
////////////////////////////////////////////////////////////////////
void PPDirectory::
read_file_dependencies(const string &cache_filename) {
  // Open up the dependency cache file in the directory.
  string cache_pathname = get_path() + "/" + cache_filename;
  ifstream in(cache_pathname.c_str());
  if (!in) {
    // Can't read it.  Maybe it's not there.  No problem.
    return;
  }

  string line;
  getline(in, line);
  while (!in.fail() && !in.eof()) {
    vector<string> words;
    tokenize_whitespace(line, words);
    if (words.size() >= 2) {
      PPDependableFile *file = get_dependable_file(words[0], false);
      file->update_from_cache(words);
    }
    getline(in, line);
  }

  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    (*ci)->read_file_dependencies(cache_filename);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::update_file_dependencies
//       Access: Private
//  Description: After all source processing has completed, makes one
//               more pass through the directory hierarchy and writes
//               out the inter-file dependency cache.
////////////////////////////////////////////////////////////////////
void PPDirectory::
update_file_dependencies(const string &cache_filename) {
  // Open up the dependency cache file in the directory.
  string cache_pathname = get_path() + "/" + cache_filename;
  unlink(cache_pathname.c_str());

  // If we have no files, don't bother writing the cache.
  if (!_dependables.empty()) {
    bool wrote_anything = false;

    ofstream out(cache_pathname.c_str(), ios::out, 0666);
    if (!out) {
      cerr << "Cannot update cache dependency file " << cache_pathname << "\n";
      return;
    }
    
    // Walk through our list of dependable files, writing them out the
    // the cache file.
    Dependables::const_iterator di;
    for (di = _dependables.begin(); di != _dependables.end(); ++di) {
      PPDependableFile *file = (*di).second;
      if (file->was_examined()) {
	if (file->is_circularity()) {
	  cerr << "Warning: circular #include directives:\n"
	       << "  " << file->get_circularity() << "\n";
	}
	file->write_cache(out);
	wrote_anything = true;
      }
    }

    out.close();

    if (!wrote_anything) {
      // Well, if we didn't write anything, remove the cache file
      // after all.
      unlink(cache_pathname.c_str());
    }
  }

  Children::iterator ci;
  for (ci = _children.begin(); ci != _children.end(); ++ci) {
    (*ci)->update_file_dependencies(cache_filename);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_complete_i_depend_on
//       Access: Private
//  Description: Gets the transitive closure of i_depend_on.  This
//               fills the given set (which must have been empty
//               before this call) with the complete set of all
//               directories this directory depends on, directly or
//               indirectly.
////////////////////////////////////////////////////////////////////
void PPDirectory::
get_complete_i_depend_on(Depends &dep) const {
  Depends::const_iterator di;
  for (di = _i_depend_on.begin(); di != _i_depend_on.end(); ++di) {
    PPDirectory *dir = (*di);
    bool inserted = dep.insert(dir).second;
    if (inserted) {
      dir->get_complete_i_depend_on(dep);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::get_complete_depends_on_me
//       Access: Private
//  Description: Gets the transitive closure of depends_on_me.  This
//               fills the given set (which must have been empty
//               before this call) with the complete set of all
//               directories this that depend on this directory,
//               directly or indirectly.
////////////////////////////////////////////////////////////////////
void PPDirectory::
get_complete_depends_on_me(Depends &dep) const {
  Depends::const_iterator di;
  for (di = _depends_on_me.begin(); di != _depends_on_me.end(); ++di) {
    PPDirectory *dir = (*di);
    bool inserted = dep.insert(dir).second;
    if (inserted) {
      dir->get_complete_depends_on_me(dep);
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPDirectory::show_directories
//       Access: Private
//  Description: Writes a set of dependency directory names to
//               standard error.  The output begins with a newline.
////////////////////////////////////////////////////////////////////
void PPDirectory::
show_directories(const PPDirectory::Depends &dep) const {
  // Copy the set into a vector, so we can sort it into a nice order
  // for the user's pleasure.
  vector<PPDirectory *> dirs;
  copy(dep.begin(), dep.end(),
       back_insert_iterator<vector<PPDirectory *> >(dirs));
  
  sort(dirs.begin(), dirs.end(), SortDirectoriesByDependencyAndName());
  
  static const int max_col = 72;
  int col = max_col;
  vector<PPDirectory *>::const_iterator di;
  for (di = dirs.begin(); di != dirs.end(); ++di) {
    const string &dirname = (*di)->_dirname;
    col += dirname.length() + 1;
    if (col >= max_col) {
      col = dirname.length() + 2;
      cerr << "\n  " << dirname;
    } else {
      cerr << " " << dirname;
    }
  }
  cerr << "\n";
}