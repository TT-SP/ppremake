// Filename: ppDependableFile.h
// Created by:  drose (15Oct00)
// 
////////////////////////////////////////////////////////////////////

#ifndef PPDEPENDABLEFILE_H
#define PPDEPENDABLEFILE_H

#include "ppremake.h"

#include <set>
#include <vector>

class PPDirectory;

///////////////////////////////////////////////////////////////////
// 	 Class : PPDependableFile
// Description : Corresponds to a single C/C++ source file, either a
//               .c file or a .h file, that can be scanned for a
//               number of #include statements.  This file may both
//               depend on other files, as well as being depended upon
//               in turn.  This is used to resolved inter-file
//               dependencies.
////////////////////////////////////////////////////////////////////
class PPDependableFile {
public:
  PPDependableFile(PPDirectory *directory, const string &filename);
  void update_from_cache(const vector<string> &words);
  void write_cache(ostream &out);

  PPDirectory *get_directory() const;
  const string &get_filename() const;
  string get_pathname() const;
  string get_dirpath() const;

  bool exists();
  time_t get_mtime();

  int get_num_dependencies();
  PPDependableFile *get_dependency(int n);

  void get_complete_dependencies(set<PPDependableFile *> &files);

  bool is_circularity();
  string get_circularity();

  bool was_examined() const;

private:
  void update_dependencies();
  PPDependableFile *compute_dependencies(string &circularity);
  void stat_file();

  PPDirectory *_directory;
  string _filename;

  enum Flags {
    F_updating    = 0x001,
    F_updated     = 0x002,
    F_circularity = 0x004,
    F_statted     = 0x008,
    F_exists      = 0x010,
    F_from_cache  = 0x020,
  };
  int _flags;
  string _circularity;
  time_t _mtime;

  class Dependency {
  public:
    PPDependableFile *_file;
    bool _okcircular;
  };

  typedef vector<Dependency> Dependencies;
  Dependencies _dependencies;

  typedef vector<string> ExtraIncludes;
  ExtraIncludes _extra_includes;
};

#endif
  