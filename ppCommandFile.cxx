// Filename: ppCommandFile.cxx
// Created by:  drose (25Sep00)
// 
////////////////////////////////////////////////////////////////////

#include "ppCommandFile.h"
#include "ppScope.h"
#include "ppNamedScopes.h"
#include "ppSubroutine.h"
#include "tokenize.h"

#include <ctype.h>
#include <stdio.h>  // for tempnam()
#include <unistd.h>

static const string begin_comment(BEGIN_COMMENT);

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::WriteState::Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PPCommandFile::WriteState::
WriteState() {
  _out = &cout;
  _format = WF_collapse;
  _last_blank = true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::WriteState::Copy Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PPCommandFile::WriteState::
WriteState(const WriteState &copy) :
  _out(copy._out),
  _format(copy._format),
  _last_blank(copy._last_blank)
{
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::WriteState::write_line
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
bool PPCommandFile::WriteState::
write_line(const string &line) {
  switch (_format) {
  case WF_straight:
    (*_out) << line << "\n";
    return true;

  case WF_collapse:
    return write_collapse_line(line);

  case WF_makefile:
    return write_makefile_line(line);
  }

  cerr << "Unsupported write format: " << (int)_format << "\n";
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::WriteState::write_collapse_line
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
bool PPCommandFile::WriteState::
write_collapse_line(const string &line) {
  if (line.empty()) {
    if (!_last_blank) {
      (*_out) << "\n";
      _last_blank = true;
    }
    
  } else {
    _last_blank = false;
    (*_out) << line << "\n";
  }
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::WriteState::write_makefile_line
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
bool PPCommandFile::WriteState::
write_makefile_line(const string &line) {
  if (line.length() <= 72) {
    return write_collapse_line(line);
  }
  _last_blank = false;

  // In makefile mode, long variable assignment lines are folded after
  // the assignment.
  vector<string> words;
  tokenize_whitespace(line, words);

  if (words.size() > 2 && (words[1] == "=" || words[1] == ":")) {
    // This appears to be a variable assignment or a dependency rule;
    // fold it.
    (*_out) << words[0] << " " << words[1];
    vector<string>::const_iterator wi;
    int col = 80;
    wi = words.begin() + 2;
    while (wi != words.end()) {
      col += (*wi).length() + 1;
      if (col > 72) {
	(*_out) << " \\\n   ";
	col = 4 + (*wi).length();
      }
      (*_out) << " " << (*wi);
      ++wi;
    }
    (*_out) << "\n";

  } else {
    // This is not a variable assignment, so just write it out.
    (*_out) << line << "\n";
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PPCommandFile::
PPCommandFile(PPScope *scope) {
  _native_scope = scope;
  _scope = scope;
  _got_command = false;
  _in_for = false;
  _if_nesting = (IfNesting *)NULL;
  _block_nesting = (BlockNesting *)NULL;
  _write_state = new WriteState;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::Destructor
//       Access: Public, Virtual
//  Description: 
////////////////////////////////////////////////////////////////////
PPCommandFile::
~PPCommandFile() {
  delete _write_state;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::set_output
//       Access: Public
//  Description: Changes the main output stream that will be written
//               to when text appears outside of a #output .. #end
//               block.  This is cout by default.
////////////////////////////////////////////////////////////////////
void PPCommandFile::
set_output(ostream *out) {
  _write_state->_out = out;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::set_scope
//       Access: Public
//  Description: Changes the command file to use the indicated scope.
//               This scope will *not* be deleted when the command
//               file destructs.
////////////////////////////////////////////////////////////////////
void PPCommandFile::
set_scope(PPScope *scope) {
  _scope = scope;
  _native_scope = scope;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::get_scope
//       Access: Public
//  Description: Returns the current scope associated with the command
//               file.  This may change as the command file is
//               processed (e.g. between #begin .. #end sequences),
//               and it may or may not be tied to the life of the
//               PPCommandFile itself.
////////////////////////////////////////////////////////////////////
PPScope *PPCommandFile::
get_scope() const {
  return _scope;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::read_file
//       Access: Public
//  Description: Reads input from the given filename.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
read_file(const string &filename) {
  ifstream in(filename.c_str());

  if (!in) {
    cerr << "Unable to open " << filename << ".\n";
    return false;
  }

  PushFilename pushed(_scope, filename);

  if (!read_stream(in)) {
    if (!in.eof()) {
      cerr << "Error reading " << filename << ".\n";
    }
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::read_stream
//       Access: Public
//  Description: Reads input from the given stream.  Each line is
//               read, commands are processed, variables are expanded,
//               and the resulting output is sent to write_line()
//               one line at a time.  The return value is true if the
//               entire file is read with no errors, false if there is
//               some problem.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
read_stream(istream &in) {
  string line;
  getline(in, line);
  begin_read();
  while (!in.fail() && !in.eof()) {
    if (!read_line(line)) {
      return false;
    }
    getline(in, line);
  }

  if (!end_read()) {
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::begin_read
//       Access: Public
//  Description: Resets to the beginning-of-the-stream state, in
//               preparation for a sequence of read_line() calls.
////////////////////////////////////////////////////////////////////
void PPCommandFile::
begin_read() {
  assert(_if_nesting == (IfNesting *)NULL);
  assert(_block_nesting == (BlockNesting *)NULL);
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::read_line
//       Access: Public
//  Description: Reads one line at a time, as if from the input
//               stream.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
read_line(string line) {
  // First things first: strip off any comment in the line.

  // We only recognize comments that are proceeded by whitespace, or
  // that start at the beginning of the line.
  size_t comment = line.find(begin_comment);
  while (comment != string::npos && 
	 !(comment == 0 || isspace(line[comment - 1]))) {
    comment = line.find(begin_comment, comment + begin_comment.length());
  }

  if (comment != string::npos) {
    // Also strip any whitespace leading up to the comment.
    while (comment > 0 && isspace(line[comment - 1])) {
      comment--;
    }
    line = line.substr(0, comment);
  }

  // If the comment was at the beginning of the line, ignore the whole
  // line, including its whitespace.
  if (comment != 0) {
    if (_in_for) {
      // Save up the lines for later execution if we're within a #forscopes.
      _saved_lines.push_back(line);
    }
    
    if (_got_command) {
      return handle_command(line);
      
    } else {
      // Find the beginning of the line--skip initial whitespace.
      size_t p = 0;
      while (p < line.length() && isspace(line[p])) {
	p++;
      }
      
      if (p == line.length()) {
	// The line is empty.  Make it truly empty.
	line = "";
	
      } else {
	if (p + 1 < line.length() && line[p] == COMMAND_PREFIX && 
	    isalpha(line[p + 1])) {
	  // This is a special command.
	  return handle_command(line.substr(p + 1));
	}
      }
      
      if (!_in_for && !failed_if()) {
	return _write_state->write_line(_scope->expand_string(line));
      }
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::end_read
//       Access: Public
//  Description: Finishes up the input stream, after a sequence of
//               read_line() calls.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
end_read() {
  bool okflag = true;

  if (_if_nesting != (IfNesting *)NULL) {
    cerr << "Unclosed if\n";
    _if_nesting = (IfNesting *)NULL;
    okflag = false;
  }

  if (_block_nesting != (BlockNesting *)NULL) {
    switch (_block_nesting->_state) {
    case BS_begin:
      cerr << "Unclosed begin " << _block_nesting->_name << "\n";
      break;

    case BS_forscopes:
    case BS_nested_forscopes:
      cerr << "Unclosed forscopes " << _block_nesting->_name << "\n";
      break;

    case BS_foreach:
    case BS_nested_foreach:
      cerr << "Unclosed foreach " << _block_nesting->_name << "\n";
      break;

    case BS_formap:
    case BS_nested_formap:
      cerr << "Unclosed formap " << _block_nesting->_name << "\n";
      break;

    case BS_defsub:
      cerr << "Unclosed defsub " << _block_nesting->_name << "\n";
      break;

    case BS_defun:
      cerr << "Unclosed defun " << _block_nesting->_name << "\n";
      break;

    case BS_output:
      cerr << "Unclosed output " << _block_nesting->_name << "\n";
      break;
    }
    _block_nesting = (BlockNesting *)NULL;
    okflag = false;
  }

  return okflag;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_command
//       Access: Protected
//  Description: Handles a macro command.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_command(const string &line) {
  if (_got_command) {
    // If we were still processing a command from last time, keep
    // going; this line is just a continuation.
    _params += line;

  } else {
    // This is the first line of a new command.

    // Pull off the first word and the rest of the line.
    size_t p = 0;
    while (p < line.length() && !isspace(line[p])) {
      p++;
    }
    _command = line.substr(0, p);
  
    // Skip whitespace between the command and its arguments.
    while (p < line.length() && isspace(line[p])) {
      p++;
    }
    _params = line.substr(p);
  }

  if (_params[_params.length() - 1] == '\\') {
    // If the line ends with a backslash, there's more to come before
    // we can process the command.
    _got_command = true;
    _params[_params.length() - 1] = ' ';
    return true;
  }

  // We're completely done scanning the command now.
  _got_command = false;

  if (_command == "if") {
    return handle_if_command();

  } else if (_command == "elif") {
    return handle_elif_command();
  
  } else if (_command == "else") {
    return handle_else_command();

  } else if (_command == "endif") {
    return handle_endif_command();

  } else if (failed_if()) {
    // If we're in the middle of a failed #if, we ignore all commands
    // except for the if-related commands, above.
    return true;

  } else if (_command == "begin") {
    return handle_begin_command();

  } else if (_command == "forscopes") {
    return handle_forscopes_command();

  } else if (_command == "foreach") {
    return handle_foreach_command();

  } else if (_command == "formap") {
    return handle_formap_command();

  } else if (_command == "format") {
    return handle_format_command();

  } else if (_command == "output") {
    return handle_output_command();

  } else if (_command == "print") {
    return handle_print_command();

  } else if (_command == "defsub") {
    return handle_defsub_command(true);

  } else if (_command == "defun") {
    return handle_defsub_command(false);

  } else if (_command == "end") {
    return handle_end_command();

  } else if (_in_for) {
    // If we're saving up #forscopes commands, we ignore any following
    // commands for now.
    return true;

  } else if (_command == "include") {
    return handle_include_command();

  } else if (_command == "sinclude") {
    return handle_sinclude_command();

  } else if (_command == "call") {
    return handle_call_command();

  } else if (_command == "error") {
    return handle_error_command();

  } else if (_command == "defer") {
    return handle_defer_command();

  } else if (_command == "define") {
    return handle_define_command();

  } else if (_command == "set") {
    return handle_set_command();

  } else if (_command == "map") {
    return handle_map_command();

  } else if (_command == "addmap") {
    return handle_addmap_command();
  }
   
  cerr << "Invalid command: " << COMMAND_PREFIX << _command << "\n";
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_if_command
//       Access: Protected
//  Description: Handles the #if command: conditionally evaluate the
//               following code.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_if_command() {
  if (failed_if()) {
    // If we're *already* inside a failed if, we don't have to
    // evaluate this one, but we do need to record the nesting level.

    IfNesting *nest = new IfNesting;
    nest->_state = IS_done;
    nest->_next = _if_nesting;
    _if_nesting = nest;

  } else {

    // If the parameter string evaluates to empty, the case is false.
    // Otherwise the case is true.  However, if we're currently
    // scanning #forscopes or something, we don't evaluate this at
    // all, because it doesn't matter.
    if (!_in_for) {
      _params = _scope->expand_string(_params);
    }
    
    bool is_empty = true;
    string::const_iterator si;
    for (si = _params.begin(); si != _params.end() && is_empty; ++si) {
      is_empty = isspace(*si);
    }
    
    IfNesting *nest = new IfNesting;
    nest->_state = is_empty ? IS_off : IS_on;
    nest->_next = _if_nesting;
    _if_nesting = nest;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_elif_command
//       Access: Protected
//  Description: Handles the #elif command: conditionally evaluate
//               the following code, following a failed #if command.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_elif_command() {
  if (_if_nesting == (IfNesting *)NULL) {
    cerr << "elif encountered without if.\n";
    return false;
  }
  if (_if_nesting->_state == IS_else) {
    cerr << "elif encountered after else.\n";
    return false;
  }
  if (_if_nesting->_state == IS_on || _if_nesting->_state == IS_done) {
    // If we passed the #if above, we don't need to evaluate the #elif.
    _if_nesting->_state = IS_done;
    return true;
  }

  // If the parameter string evaluates to empty, the case is false.
  // Otherwise the case is true.
  if (!_in_for) {
    _params = _scope->expand_string(_params);
  }

  bool is_empty = true;
  string::const_iterator si;
  for (si = _params.begin(); si != _params.end() && is_empty; ++si) {
    is_empty = isspace(*si);
  }

  _if_nesting->_state = is_empty ? IS_off : IS_on;

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_else_command
//       Access: Protected
//  Description: Handles the #else command: evaluate the following
//               code following a failed #if command.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_else_command() {
  if (_if_nesting == (IfNesting *)NULL) {
    cerr << "else encountered without if.\n";
    return false;
  }
  if (_if_nesting->_state == IS_else) {
    cerr << "else encountered after else.\n";
    return false;
  }
  if (_if_nesting->_state == IS_on || _if_nesting->_state == IS_done) {
    _if_nesting->_state = IS_done;
    return true;
  }

  _if_nesting->_state = IS_else;
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_endif_command
//       Access: Protected
//  Description: Handles the #endif command: close a preceeding #if
//               command.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_endif_command() {
  if (_if_nesting == (IfNesting *)NULL) {
    cerr << "endif encountered without if.\n";
    return false;
  }

  IfNesting *nest = _if_nesting;
  _if_nesting = _if_nesting->_next;
  delete nest;

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_begin_command
//       Access: Protected
//  Description: Handles the #begin command: begin a named scope
//               block.  The variables defined between this command
//               and the corresponding #end command will be local to
//               this named scope.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_begin_command() {
  BlockNesting *nest = new BlockNesting;
  nest->_state = BS_begin;
  nest->_name = trim_blanks(_scope->expand_string(_params));
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;

  if (contains_whitespace(nest->_name)) {
    cerr << "Attempt to define scope named \"" << nest->_name 
	 << "\".\nScope names may not contain whitespace.\n";
    return false;
  }

  if (nest->_name.find(SCOPE_DIRNAME_SEPARATOR) != string::npos) {
    cerr << "Attempt to define scope named \"" << nest->_name 
	 << "\".\nScope names may not contain the '"
	 << SCOPE_DIRNAME_SEPARATOR << "' character.\n";
    return false;
  }

  _block_nesting = nest;

  if (nest->_name == "global") {
    // There's a special case for the named scope "global": this
    // refers to the global scope, allowing us to define macros
    // etc. that all scopes can see.
    _scope = PPScope::get_bottom_scope();

  } else {
    PPScope *named_scope = _scope->get_named_scopes()->make_scope(nest->_name);
    named_scope->set_parent(_scope);
    _scope = named_scope;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_forscopes_command
//       Access: Protected
//  Description: Handles the #forscopes command: interpret all the lines
//               between this command and the corresponding #end
//               command once for each occurrence of a named scope
//               with the given name.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_forscopes_command() {
  BlockNesting *nest = new BlockNesting;
  nest->_state = _in_for ? BS_nested_forscopes : BS_forscopes;
  nest->_name = trim_blanks(_scope->expand_string(_params));
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;

  _block_nesting = nest;

  if (!_in_for) {
    _in_for = true;
    _saved_lines.clear();
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_foreach_command
//       Access: Protected
//  Description: Handles the #foreach command: interpret all the lines
//               between this command and the corresponding #end
//               command once for each word in the argument.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_foreach_command() {
  // Get the parameters of the foreach command.  The first word is the
  // name of the variable to substitute in (and which should appear on
  // the matching #end command), and the remaining words are the
  // values to substitute in.
  vector<string> words;
  tokenize_whitespace(_scope->expand_string(_params), words);

  if (words.empty()) {
    cerr << "#foreach requires at least one parameter.\n";
    return false;
  }

  string variable_name = words.front();

  BlockNesting *nest = new BlockNesting;
  nest->_state = _in_for ? BS_nested_foreach : BS_foreach;
  nest->_name = variable_name;
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;

  // We insert in all but the first word in the words vector.
  nest->_words.insert(nest->_words.end(), words.begin() + 1, words.end());

  _block_nesting = nest;

  if (!_in_for) {
    _in_for = true;
    _saved_lines.clear();
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_formap_command
//       Access: Protected
//  Description: Handles the #formap command: interpret all the lines
//               between this command and the corresponding #end
//               command once for each key in the map, and also within
//               the corresponding scope of that particular key.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_formap_command() {
  // Get the parameters of the formap command.  The first word is the
  // name of the variable to substitute in (and which should appear on
  // the matching #end command), and the second word is the name of
  // the map variable.
  vector<string> words;
  tokenize_whitespace(_scope->expand_string(_params), words);

  if (words.size() != 2) {
    cerr << "#formap requires exactly two parameters.\n";
    return false;
  }

  string variable_name = words.front();

  BlockNesting *nest = new BlockNesting;
  nest->_state = _in_for ? BS_nested_formap : BS_formap;
  nest->_name = words[0];
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;

  nest->_words.push_back(words[1]);

  _block_nesting = nest;

  if (!_in_for) {
    _in_for = true;
    _saved_lines.clear();
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_format_command
//       Access: Protected
//  Description: Handles the #format command: change the formatting
//               mode of lines as they are output.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_format_command() {
  _params = trim_blanks(_scope->expand_string(_params));
  if (_params == "straight") {
    _write_state->_format = WF_straight;

  } else if (_params == "collapse") {
    _write_state->_format = WF_collapse;

  } else if (_params == "makefile") {
    _write_state->_format = WF_makefile;

  } else {
    cerr << "Ignoring invalid write format: " << _params << "\n";
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_output_command
//       Access: Protected
//  Description: Handles the #output command: all text between this
//               command and the corresponding #end command will be
//               sent to the indicated output file.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_output_command() {
  BlockNesting *nest = new BlockNesting;
  nest->_state = BS_output;
  nest->_name = trim_blanks(_scope->expand_string(_params));
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;

  _block_nesting = nest;

  if (!_in_for) {
    string filename = nest->_name;
    if (filename.empty()) {
      cerr << "Attempt to output to empty filename\n";
      return false;
    }
    
    string prefix = _scope->expand_variable("DIRPREFIX");
    if (filename[0] != '/') {
      filename = prefix + filename;
    }
    
    nest->_true_name = filename;
    nest->_tempnam = (char *)NULL;

    if (access(filename.c_str(), F_OK) == 0) {
      // If the file already exists, create a temporary file first.
      
      nest->_tempnam = tempnam((prefix + ".").c_str(), "pptmp");
      assert(nest->_tempnam != (char *)NULL);
      
      nest->_output.open(nest->_tempnam);
      if (nest->_output.fail()) {
	cerr << "Unable to open output file " << nest->_tempnam << "\n";
	return false;
      }
      
    } else {
      // If the file does not already exist, create it directly instead
      // of monkeying around with temporary files.
      cerr << "Generating " << filename << "\n";
      
      nest->_output.open(filename.c_str(), ios::out, 0666);
      if (nest->_output.fail()) {
	cerr << "Unable to open output file " << filename << "\n";
	return false;
      }
    }
    
    _write_state = new WriteState(*_write_state);
    _write_state->_out = &nest->_output;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_print_command
//       Access: Protected
//  Description: Handles the #print command: immediately output the
//               arguments to this line to standard error.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_print_command() {
  if (!_in_for) {
    cerr << _scope->expand_string(_params) << "\n";
  }
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_defsub_command
//       Access: Protected
//  Description: Handles the #defsub (or #defun) command: save all the
//               lines between this command and the matching #end as a
//               callable subroutine to be invoked by a later #call
//               command.  If is_defsub is false, it means this
//               subroutine was actually defined via a #defun command,
//               so it is to be invoked by a later variable reference,
//               instead of by a #call command.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_defsub_command(bool is_defsub) {
  string command = (is_defsub) ? "#defsub" : "#defun";

  // The first word of the parameter list is the subroutine name; the
  // rest is the comma-separated list of formal parameter names.

  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string subroutine_name = trim_blanks(_params.substr(0, p));

  if (subroutine_name.empty()) {
    cerr << command << " requires at least one parameter.\n";
    return false;
  }

  vector<string> formals;
  _scope->tokenize_params(_params.substr(p), formals, false);

  vector<string>::const_iterator fi;
  for (fi = formals.begin(); fi != formals.end(); ++fi) {
    if (!is_valid_formal(*fi)) {
      cerr << command << " " << subroutine_name
	   << ": invalid formal parameter name '" << (*fi) << "'\n";
      return false;
    }
  }

  if (_in_for) {
    cerr << command << " may not appear within another block scoping command like\n"
	 << "#forscopes, #foreach, #formap, #defsub, or #defun.\n";
    return false;
  }

  BlockNesting *nest = new BlockNesting;
  nest->_state = is_defsub ? BS_defsub : BS_defun;
  nest->_name = subroutine_name;
  nest->_write_state = _write_state;
  nest->_scope = _scope;
  nest->_next = _block_nesting;
  nest->_words.swap(formals);

  _block_nesting = nest;

  _in_for = true;
  _saved_lines.clear();

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_end_command
//       Access: Protected
//  Description: Handles the #end command.  This closes a number of
//               different kinds of blocks, like #begin and #forscopes.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_end_command() {
  if (_block_nesting == (BlockNesting *)NULL) {
    cerr << "Unmatched end " << _params << ".\n";
    return false;
  }

  string name = trim_blanks(_scope->expand_string(_params));
  if (name != _block_nesting->_name) {
    cerr << "end " << name << " encountered where end "
	 << _block_nesting->_name << " expected.\n";
    return false;
  }

  BlockNesting *nest = _block_nesting;

  _scope = nest->_scope;
  if (_write_state != nest->_write_state) {
    delete _write_state;
    _write_state = nest->_write_state;
  }

  _block_nesting = nest->_next;

  if (nest->_state == BS_forscopes) {
    // Now replay all of the saved lines.
    _in_for = false;
    if (!replay_forscopes(nest->_name)) {
      return false;
    }

  } else if (nest->_state == BS_foreach) {
    // Now replay all of the saved lines.
    _in_for = false;
    if (!replay_foreach(nest->_name, nest->_words)) {
      return false;
    }

  } else if (nest->_state == BS_formap) {
    // Now replay all of the saved lines.
    _in_for = false;
    assert(nest->_words.size() == 1);
    if (!replay_formap(nest->_name, nest->_words[0])) {
      return false;
    }

  } else if (nest->_state == BS_defsub || nest->_state == BS_defun) {
    // Save all of the saved lines as a named subroutine.
    _in_for = false;
    PPSubroutine *sub = new PPSubroutine;
    sub->_formals.swap(nest->_words);
    sub->_lines.swap(_saved_lines);

    // Remove the #end command.  This will fail if someone makes an
    // #end command that spans multiple lines.  Don't do that.
    assert(!sub->_lines.empty());
    sub->_lines.pop_back();

    if (nest->_state == BS_defsub) {
      PPSubroutine::define_sub(nest->_name, sub);
    } else {
      PPSubroutine::define_func(nest->_name, sub);
    }

  } else if (nest->_state == BS_output) {
    if (!_in_for) {
      if (!nest->_output) {
	cerr << "Error while writing " << nest->_true_name << "\n";
	return false;
      }
      nest->_output.close();

      // Verify the output file.
      if (nest->_tempnam != (char *)NULL) {
	if (!compare_output(nest->_tempnam, nest->_true_name)) {
	  return false;
	}
	free(nest->_tempnam);
      }
    }
  }


  delete nest;

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_include_command
//       Access: Protected
//  Description: Handles the #include command: the indicated file is
//               read and processed at this point.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_include_command() {
  string filename = trim_blanks(_scope->expand_string(_params));

  // We allow optional quotation marks around the filename.
  if (filename.length() >= 2 &&
      filename[0] == '"' && 
      filename[filename.length() - 1] == '"') {
    filename = filename.substr(1, filename.length() - 2);
  }

  return include_file(filename);
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_sinclude_command
//       Access: Protected
//  Description: Handles the #sinclude command: the indicated file is
//               read and processed at this point.  This is different
//               from #include only in that if the file does not
//               exist, there is no error; instead, nothing happens.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_sinclude_command() {
  string filename = trim_blanks(_scope->expand_string(_params));

  // We allow optional quotation marks around the filename.
  if (filename.length() >= 2 &&
      filename[0] == '"' && 
      filename[filename.length() - 1] == '"') {
    filename = filename.substr(1, filename.length() - 2);
  }

  if (access(filename.c_str(), F_OK) != 0) {
    // No such file; no error.
    return true;
  }

  return include_file(filename);
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_call_command
//       Access: Protected
//  Description: Handles the #call command: the indicated named
//               subroutine is read and processed at this point.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_call_command() {
  // The first word is the name of the subroutine; the rest is the
  // comma-separated list of expressions to substitute in for the
  // subroutine's formal parameters.

  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string subroutine_name = trim_blanks(_params.substr(0, p));
  string params = _params.substr(p);

  if (subroutine_name.empty()) {
    cerr << "#call requires at least one parameter.\n";
    return false;
  }

  const PPSubroutine *sub = PPSubroutine::get_sub(subroutine_name);
  if (sub == (const PPSubroutine *)NULL) {
    cerr << "Attempt to call undefined subroutine " << subroutine_name << "\n";
  }

  PPScope *old_scope = _scope;
  PPScope::push_scope(_scope);
  PPScope nested_scope(_scope->get_named_scopes());
  _scope = &nested_scope;
  nested_scope.define_formals(subroutine_name, sub->_formals, params);

  vector<string>::const_iterator li;
  for (li = sub->_lines.begin(); li != sub->_lines.end(); ++li) {
    if (!read_line(*li)) {
      PPScope::pop_scope();
      _scope = old_scope;
      return false;
    }
  }

  PPScope::pop_scope();
  _scope = old_scope;
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_error_command
//       Access: Protected
//  Description: Handles the #error command: terminate immediately
//               with the given error message.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_error_command() {
  string message = trim_blanks(_scope->expand_string(_params));
  
  if (!message.empty()) {
    cerr << message << "\n";
  }
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_defer_command
//       Access: Protected
//  Description: Handles the #defer command: define a new variable or
//               change the definition of an existing variable.  This
//               is different from #define in that the variable
//               definition is not immediately expanded; it will be
//               expanded when the variable is later used.  This
//               allows the definition of variables that depend on
//               other variables whose values have not yet been
//               defined.  This is akin to GNU make's = assignment.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_defer_command() {
  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string varname = _params.substr(0, p);

  if (PPSubroutine::get_func(varname) != (const PPSubroutine *)NULL) {
    cerr << "Warning: variable " << varname
	 << " shadowed by function definition.\n";
  }
  
  // Skip whitespace between the variable name and its definition.
  while (p < _params.length() && isspace(_params[p])) {
    p++;
  }
  string def = _params.substr(p);

  // We don't expand the variable's definition immediately; it will be
  // expanded when the variable is referenced later.  However, we
  // should expand any simple self-reference immediately, to allow for
  // recursive definitions.
  def = _scope->expand_self_reference(def, varname);

  _scope->define_variable(varname, def);

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_define_command
//       Access: Protected
//  Description: Handles the #define command: define a new variable or
//               change the definition of an existing variable.  The
//               variable definition is immediately expanded.  This is
//               akin to GNU make's := assignment.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_define_command() {
  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string varname = _params.substr(0, p);

  if (PPSubroutine::get_func(varname) != (const PPSubroutine *)NULL) {
    cerr << "Warning: variable " << varname
	 << " shadowed by function definition.\n";
  }
  
  // Skip whitespace between the variable name and its definition.
  while (p < _params.length() && isspace(_params[p])) {
    p++;
  }
  string def = _scope->expand_string(_params.substr(p));
  _scope->define_variable(varname, def);

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_set_command
//       Access: Protected
//  Description: Handles the #set command: change the definition of an
//               existing variable.
//
//               This is different from #defer in two ways: (1) the
//               variable in question must already have been #defined
//               elsewhere, (2) if the variable was #defined in some
//               parent scope, this will actually change the variable
//               in the parent scope, rather than shadowing it in the
//               local scope.  Like #define and unlike #defer, the
//               variable definition is expanded immediately, similar
//               to GNU make's := operator.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_set_command() {
  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string varname = _params.substr(0, p);

  if (PPSubroutine::get_func(varname) != (const PPSubroutine *)NULL) {
    cerr << "Warning: variable " << varname
	 << " shadowed by function definition.\n";
  }
  
  // Skip whitespace between the variable name and its definition.
  while (p < _params.length() && isspace(_params[p])) {
    p++;
  }
  string def = _scope->expand_string(_params.substr(p));

  if (!_scope->set_variable(varname, def)) {
    cerr << "Attempt to set undefined variable " << varname << "\n";
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_map_command
//       Access: Protected
//  Description: Handles the #map command: define a new map variable.
//               This is a special kind of variable declaration that
//               creates a variable that can be used as a function to
//               look up variable expansions within a number of
//               different named scopes, accessed by keyword.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_map_command() {
  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string varname = _params.substr(0, p);
  
  // Skip whitespace between the variable name and its definition.
  while (p < _params.length() && isspace(_params[p])) {
    p++;
  }
  string def = trim_blanks(_params.substr(p));

  _scope->define_map_variable(varname, def);
  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::handle_addmap_command
//       Access: Protected
//  Description: Handles the #addmap command: add a new key/scope pair
//               to an existing map variable.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
handle_addmap_command() {
  // Pull off the first word and the rest of the params.
  size_t p = 0;
  while (p < _params.length() && !isspace(_params[p])) {
    p++;
  }
  string varname = _params.substr(0, p);
  
  // Skip whitespace between the variable name and the key.
  while (p < _params.length() && isspace(_params[p])) {
    p++;
  }
  string key = trim_blanks(_scope->expand_string(_params.substr(p)));

  _scope->add_to_map_variable(varname, key, _scope);
  return true;
}


////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::include_file
//       Access: Protected
//  Description: The internal implementation of #include: includes a
//               particular named file at this point.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
include_file(const string &filename) {
  ifstream in(filename.c_str());

  if (!in) {
    cerr << "Unable to open include file " << filename << ".\n";
    return false;
  }

  PushFilename pushed(_scope, filename);

  string line;
  getline(in, line);
  while (!in.fail() && !in.eof()) {
    if (!read_line(line)) {
      return false;
    }
    getline(in, line);
  }

  if (!in.eof()) {
    cerr << "Error reading " << filename << ".\n";
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::replay_forscopes
//       Access: Protected
//  Description: Replays all the lines that were saved during a
//               previous #forscopes..#end block.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
replay_forscopes(const string &name) {
  bool okflag = true;

  vector<string> lines;
  lines.swap(_saved_lines);

  // Remove the #end command.  This will fail if someone makes an #end
  // command that spans multiple lines.  Don't do that.
  assert(!lines.empty());
  lines.pop_back();

  PPNamedScopes *named_scopes = _scope->get_named_scopes();

  // Extract out the scope names from the #forscopes .. #end name.  This
  // is a space-delimited list of scope names.
  vector<string> words;
  tokenize_whitespace(name, words);

  // Now build up the list of scopes with these names.
  PPNamedScopes::Scopes scopes;
  vector<string>::const_iterator wi;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    named_scopes->get_scopes(*wi, scopes);
  }
  PPNamedScopes::sort_by_dependency(scopes);
    
  PPNamedScopes::Scopes::const_iterator si;
  for (si = scopes.begin(); si != scopes.end(); ++si) {
    PPScope::push_scope(_scope);
    _scope = (*si);
    
    vector<string>::const_iterator li;
    for (li = lines.begin(); li != lines.end() && okflag; ++li) {
      okflag = read_line(*li);
    }
    _scope = PPScope::pop_scope();
  }

  return okflag;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::replay_foreach
//       Access: Protected
//  Description: Replays all the lines that were saved during a
//               previous #foreach..#end block.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
replay_foreach(const string &varname, const vector<string> &words) {
  bool okflag = true;

  vector<string> lines;
  lines.swap(_saved_lines);

  // Remove the #end command.  This will fail if someone makes an #end
  // command that spans multiple lines.  Don't do that.
  assert(!lines.empty());
  lines.pop_back();

  // Now traverse through the saved words.
  vector<string>::const_iterator wi;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    _scope->define_variable(varname, (*wi));
    vector<string>::const_iterator li;
    for (li = lines.begin(); li != lines.end() && okflag; ++li) {
      okflag = read_line(*li);
    }
  }

  return okflag;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::replay_formap
//       Access: Protected
//  Description: Replays all the lines that were saved during a
//               previous #formap..#end block.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
replay_formap(const string &varname, const string &mapvar) {
  bool okflag = true;

  vector<string> lines;
  lines.swap(_saved_lines);

  // Remove the #end command.  This will fail if someone makes an #end
  // command that spans multiple lines.  Don't do that.
  assert(!lines.empty());
  lines.pop_back();

  // Now look up the map variable.
  PPScope::MapVariableDefinition &def = _scope->find_map_variable(mapvar);
  if (&def == &PPScope::_null_map_def) {
    cerr << "Undefined map variable: #formap " << varname << " " 
	 << mapvar << "\n";
    return false;
  }

  // Now traverse through the map definition.
  PPScope::MapVariableDefinition::const_iterator di;
  for (di = def.begin(); di != def.end() && okflag; ++di) {
    _scope->define_variable(varname, (*di).first);

    PPScope::push_scope(_scope);
    _scope = (*di).second;

    vector<string>::const_iterator li;
    for (li = lines.begin(); li != lines.end() && okflag; ++li) {
      okflag = read_line(*li);
    }

    _scope = PPScope::pop_scope();
  }

  return okflag;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::compare_output
//       Access: Protected
//  Description: After a temporary file has been written due to an
//               #output command, compare the results to the original
//               file.  If they are different, remove the original
//               file and rename the temporary file; if they are the
//               same, remove the temporary file.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
compare_output(const string &temp_name, const string &true_name) {
  ifstream in_a(temp_name.c_str());
  ifstream in_b(true_name.c_str());

  int a = in_a.get();
  int b = in_b.get(); 
  bool differ = (a != b);
  while (!in_a.eof() && !in_b.eof() && !differ) {
    a = in_a.get();
    b = in_b.get(); 
    differ = (a != b);
  }

  in_a.close();
  in_b.close();

  if (differ) {
    cerr << "Generating " << true_name << "\n";

    if (unlink(true_name.c_str()) < 0) {
      cerr << "Unable to remove old " << true_name << "\n";
      return false;
    }

    if (rename(temp_name.c_str(), true_name.c_str()) < 0) {
      cerr << "Unable to rename temporary file " << temp_name
	   << " to " << true_name << "\n";
      return false;
    }

  } else {
    //    cerr << "File " << true_name << " is unchanged.\n";
    if (unlink(temp_name.c_str()) < 0) {
      cerr << "Warning: unable to remove temporary file " << temp_name << "\n";
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::failed_if
//       Access: Protected
//  Description: Returns true if we are currently within a failed #if
//               block (or in an #else block for a passed #if block),
//               or false otherwise.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
failed_if() const {
  return (_if_nesting != (IfNesting *)NULL && 
	  (_if_nesting->_state == IS_off || _if_nesting->_state == IS_done));
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::is_valid_formal_parameter_name
//       Access: Protected
//  Description: Returns true if the indicated name is an acceptable
//               name for a formal parameter.  This means it includes
//               no whitespace or crazy punctuation.  Mainly this is
//               to protect the user from making some stupid syntax
//               mistake.
////////////////////////////////////////////////////////////////////
bool PPCommandFile::
is_valid_formal(const string &formal_parameter_name) const {
  if (formal_parameter_name.empty()) {
    return false;
  }
  
  string::const_iterator si;
  for (si = formal_parameter_name.begin();
       si != formal_parameter_name.end();
       ++si) {
    switch (*si) {
    case ' ':
    case '\n':
    case '\t':
    case '$':
    case '[':
    case ']':
    case ',':
      return false;
    }
  }

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::PushFilename::Constructor
//       Access: Public
//  Description: This special class changes the current filename of
//               the PPCommandFile.  The idea is to create one of
//               these when the filename is changed (for instance, to
//               read in a new file via an #include directive); when
//               the variable then goes out of scope, it will restore
//               the previous filename.
//
//               This updates the scope with the appropriate
//               variables.
////////////////////////////////////////////////////////////////////
PPCommandFile::PushFilename::
PushFilename(PPScope *scope, const string &filename) {
  _scope = scope;
  _old_thisdirprefix = _scope->get_variable("THISDIRPREFIX");
  _old_thisfilename = _scope->get_variable("THISFILENAME");

  _scope->define_variable("THISFILENAME", filename);
  size_t slash = filename.rfind('/');
  if (slash == string::npos) {
    _scope->define_variable("THISDIRPREFIX", string());
  } else {
    _scope->define_variable("THISDIRPREFIX", filename.substr(0, slash + 1));
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPCommandFile::PushFilename::Destructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PPCommandFile::PushFilename::
~PushFilename() {
  _scope->define_variable("THISDIRPREFIX", _old_thisdirprefix);
  _scope->define_variable("THISFILENAME", _old_thisfilename);
}