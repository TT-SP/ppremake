// Filename: ppremake.cxx
// Created by:  drose (25Sep00)
// 
////////////////////////////////////////////////////////////////////

#include "ppremake.h"
#include "ppMain.h"
#include "ppScope.h"
#include "check_include.h"
#include "tokenize.h"
#include "sedProcess.h"

#ifdef HAVE_GETOPT
#include <getopt.h>
#else
#include "gnu_getopt.h"
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <set>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <assert.h>

bool unix_platform = false;
bool windows_platform = false;

static void
usage() {
  cerr <<
    "\n"
    "ppremake [opts] subdir-name [subdir-name..]\n"
    "ppremake\n"
    "ppremake -s 'sed-command' <input >output\n"
    "\n"
    "This is Panda pre-make: a script preprocessor that scans the source\n"
    "directory hierarchy containing the current directory, looking for\n"
    "directories that contain a file called " SOURCE_FILENAME ".  At the top of the\n"
    "directory tree must be a file called " PACKAGE_FILENAME ", which should define\n"
    "key variable definitions for processing, as well as pointing out the\n"
    "locations of further config files.\n\n"

    "The package file is read and interpreted, followed by each source file\n"
    "in turn; after each source file is read, the template file (specified in\n"
    "the config file) is read.  The template file contains the actual statements\n"
    "to be output and will typically be set up to generate Makefiles or whatever\n"
    "is equivalent and appropriate to the particular build environment in use.\n\n"

    "The parameters are the names of the subdirectories (their local names, not\n"
    "the relative or full paths to them) that are to be processed.  All\n"
    "subdirectories (that contain a file named " SOURCE_FILENAME ") will be\n"
    "scanned, but only the named subdirectories will have output files\n"
    "generated.  If no parameter is given, then all directories will be\n"
    "processed.\n\n"

    "ppremake -s is a special form of the command that runs as a very limited\n"
    "sed.  It has nothing to do with building makefiles, but is provided mainly\n"
    "so platforms that don't have sed built in can still portably run simple sed\n"
    "scripts.\n\n"

    "Options:\n\n"

    "  -h           Display this page.\n"
    "  -V           Report the version of ppremake, and exit.\n"
    "  -P           Report the current platform name, and exit.\n\n"
    "  -D pp.dep    Examine the given dependency file, and re-run ppremake\n"
    "               only if the dependency file is stale.\n\n"
    "  -d           Instead of generating makefiles, report the set of\n"
    "               subdirectories that the named subdirectory depends on.\n"
    "               Directories are named by their local name, not by the\n"
    "               path to them; e.g. util instead of src/util.\n"
    "  -n           As above, but report the set of subdirectories that\n"
    "               depend on (need) the named subdirectory.  Options -d and\n"
    "               -n may be combined, and you may also name multiple\n"
    "               subdirectories to scan at once.\n\n"
    "  -p platform  Build as if for the indicated platform name.\n"
    "  -c config.pp Read the indicated user-level config.pp file after reading\n"
    "               the system config.pp file.  If this is omitted, the value\n"
    "               given in the environment variable PPREMAKE_CONFIG is used\n"
    "               instead.\n\n";
}

static void
report_version() {
  cerr << "This is " << PACKAGE << " version " << VERSION << ".\n";
}

static void
report_platform() {
  cerr << "ppremake built for platform " << PLATFORM << ".\n";
}

////////////////////////////////////////////////////////////////////
//     Function: check_one_file
//  Description: Checks a single file listed in the dependency cache
//               to see if it matches the cache.  Returns true if it
//               does, false if it does not.
////////////////////////////////////////////////////////////////////
static bool
check_one_file(const string &dir_prefix, const vector<string> &words) {
  assert(words.size() >= 2);

  string pathname = dir_prefix + words[0];
  time_t mtime = strtol(words[1].c_str(), (char **)NULL, 10);

  struct stat st;
  if (stat(pathname.c_str(), &st) < 0) {
    // The file doesn't even exist!
    return false;
  }

  if (st.st_mtime == mtime) {
    // The modification time matches; don't bother to read the file.
    return true;
  }

  // The modification time doesn't match, so we'll need to read the
  // file and look for #include directives.  First, get the complete
  // set of files we're expecting to find.

  set<string> expected_files;
  for (int i = 2; i < (int)words.size(); i++) {
    const string &word = words[i];
    size_t slash = word.rfind('/');
    if (slash == string::npos) {
      // Every file is expected to include a slash.
      return false;
    }
    expected_files.insert(word.substr(slash + 1));
  }

  // Now open the source file and read it for #include directives.
  set<string> found_files;
  ifstream in(pathname.c_str());
  if (!in) {
    // Can't read the file for some reason.
    return false;
  }

  string line;
  getline(in, line);
  while (!in.fail() && !in.eof()) {
    string filename = check_include(line);
    if (!filename.empty() && filename.find('/') == string::npos) {
      found_files.insert(filename);
    }
    getline(in, line);
  }
  
  // Now check that the two sets are equivalent.
  return (expected_files == found_files);
}

////////////////////////////////////////////////////////////////////
//     Function: check_dependencies
//  Description: Read in the indicated dependency cache file,
//               verifying that it is still current.  If it is stale,
//               return false; otherwise, return true.
////////////////////////////////////////////////////////////////////
static bool
check_dependencies(const string &dep_filename) {
  // Extract the directory prefix from the dependency filename.  This
  // is everything up until (and including) the last slash.
  string dir_prefix;
  size_t slash = dep_filename.rfind('/');
  if (slash != string::npos) {
    dir_prefix = dep_filename.substr(0, slash + 1);
  }

  ifstream in(dep_filename.c_str());
  if (!in) {
    // The dependency filename doesn't even exist: it's definitely
    // stale.
    return false;
  }

  string line;
  getline(in, line);
  while (!in.fail() && !in.eof()) {
    vector<string> words;
    tokenize_whitespace(line, words);
    if (words.size() < 2) {
      // Invalid file; return false.
      return false;
    }
    if (!check_one_file(dir_prefix, words)) {
      // This file is stale; return false.
      return false;
    }
    getline(in, line);
  }

  // All files are ok; return true.
  return true;
}

int
main(int argc, char *argv[]) {
  string progname = argv[0];
  extern char *optarg;
  extern int optind;
  const char *optstr = "hVPD:dnp:c:s:";

  bool any_d = false;
  bool dependencies_stale = false;
  bool report_depends = false;
  bool report_needs = false;
  string platform = PLATFORM;
  string ppremake_config;
  bool got_ppremake_config = false;
  string sed_command;
  bool got_sed_command = false;
  int flag = getopt(argc, argv, optstr);

  while (flag != EOF) {
    switch (flag) {
    case 'h':
      usage();
      exit(0);

    case 'V':
      report_version();
      exit(0);
      break;

    case 'P':
      report_platform();
      exit(0);
      break;

    case 'D':
      if (!check_dependencies(optarg)) {
        dependencies_stale = true;
      }
      any_d = true;
      break;

    case 'd':
      report_depends = true;
      break;

    case 'n':
      report_needs = true;
      break;

    case 'p':
      platform = optarg;
      break;

    case 'c':
      ppremake_config = optarg;
      got_ppremake_config = true;
      break;

    case 's':
      sed_command = optarg;
      got_sed_command = true;
      break;

    default:
      exit(1);
    }
    flag = getopt(argc, argv, optstr);
  }

  argc -= (optind-1);
  argv += (optind-1);

  if (got_sed_command) {
    SedProcess sp;
    if (!sp.add_script_line(sed_command)) {
      exit(1);
    }
    sp.run(cin, cout);
    exit(0);
  }

  // If the user supplied one or more -d parameters, then we should
  // not continue unless some of the dependencies were stale.
  if (any_d) {
    if (!dependencies_stale) {
      exit(0);
    }
    cout << progname << "\n";
  }

  PPScope global_scope((PPNamedScopes *)NULL);
  global_scope.define_variable("PPREMAKE", PACKAGE);
  global_scope.define_variable("PPREMAKE_VERSION", VERSION);
  global_scope.define_variable("PLATFORM", platform);
  global_scope.define_variable("PACKAGE_FILENAME", PACKAGE_FILENAME);
  global_scope.define_variable("SOURCE_FILENAME", SOURCE_FILENAME);

  if (got_ppremake_config) {
    // If this came in on the command line, define a variable as such.
    // Otherwise, the system scripts can pull this value in from the
    // similarly-named environment variable.
    global_scope.define_variable("PPREMAKE_CONFIG", ppremake_config);
  }

  // Also, it's convenient to have a way to represent the literal tab
  // character, without actually putting a literal tab character in
  // the source file.
  global_scope.define_variable("TAB", "\t");

  PPMain ppmain(&global_scope);
  if (!ppmain.read_source(".")) {
    exit(1);
  }

  if (report_depends || report_needs) {
    // With -d or -n, just report inter-directory dependency
    // relationships.
    if (argc < 2) {
      cerr << "No named directories.\n";
      exit(1);
    }

    for (int i = 1; i < argc; i++) {
      cerr << "\n";
      if (report_depends) {
        ppmain.report_depends(argv[i]);
      }
      cerr << "\n";
      if (report_needs) {
        ppmain.report_needs(argv[i]);
      }
    }

  } else {
    // Without -d or -n, do normal processing.

    if (argc < 2) {
      if (!ppmain.process_all()) {
        exit(1);
      }
    } else {
      for (int i = 1; i < argc; i++) {
        if (!ppmain.process(argv[i])) {
          cerr << "Unable to process " << argv[i] << ".\n";
          exit(1);
        }
      }
    }
  }

  cerr << "No errors.\n";
  return (0);
}
