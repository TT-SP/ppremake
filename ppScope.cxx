// Filename: ppScope.cxx
// Created by:  drose (25Sep00)
// 
////////////////////////////////////////////////////////////////////

#include "ppScope.h"
#include "ppNamedScopes.h"
#include "ppFilenamePattern.h"
#include "ppDirectoryTree.h"
#include "ppSubroutine.h"
#include "ppCommandFile.h"
#include "tokenize.h"
#include "find_searchpath.h"

#include <stdlib.h>
#include <algorithm>
#include <ctype.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>  // for perror() and sprintf().
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

static const string variable_patsubst(VARIABLE_PATSUBST);

PPScope::MapVariableDefinition PPScope::_null_map_def;

PPScope::ScopeStack PPScope::_scope_stack;

////////////////////////////////////////////////////////////////////
//     Function: PPScope::Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
PPScope::
PPScope(PPNamedScopes *named_scopes) : 
  _named_scopes(named_scopes)
{
  _directory = (PPDirectoryTree *)NULL;
  _parent_scope = (PPScope *)NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::get_named_scopes
//       Access: Public
//  Description: Returns a pointer to the PPNamedScopes collection
//               associated with this scope.  This pointer could be
//               NULL.
////////////////////////////////////////////////////////////////////
PPNamedScopes *PPScope::
get_named_scopes() const {
  return _named_scopes;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::set_parent
//       Access: Public
//  Description: Sets a static parent scope to this scope.  When a
//               variable reference is undefined in this scope, it
//               will search first up the static parent chain before
//               it searches the dynamic scope stack.
////////////////////////////////////////////////////////////////////
void PPScope::
set_parent(PPScope *parent) {
  _parent_scope = parent;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::get_parent
//       Access: Public
//  Description: Returns the static parent scope to this scope, if
//               any, or NULL if the static parent has not been set.
//               See set_parent().
////////////////////////////////////////////////////////////////////
PPScope *PPScope::
get_parent() const {
  return _parent_scope;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::define_variable
//       Access: Public
//  Description: Makes a new variable definition.  If the variable
//               does not already exist in this scope, a new variable
//               is created, possibly shadowing a variable declaration
//               in some parent scope.
////////////////////////////////////////////////////////////////////
void PPScope::
define_variable(const string &varname, const string &definition) {
  _variables[varname] = definition;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::set_variable
//       Access: Public
//  Description: Changes the definition of an already-existing
//               variable.  The variable is changed in whichever scope
//               it is defined.  Returns false if the variable has not
//               been defined.
////////////////////////////////////////////////////////////////////
bool PPScope::
set_variable(const string &varname, const string &definition) {
  if (p_set_variable(varname, definition)) {
    return true;
  }

  // Check the scopes on the stack for the variable definition.
  ScopeStack::reverse_iterator si;
  for (si = _scope_stack.rbegin(); si != _scope_stack.rend(); ++si) {
    if ((*si)->p_set_variable(varname, definition)) {
      return true;
    }
  }

  // If the variable isn't defined, we check the environment.
  const char *env = getenv(varname.c_str());
  if (env != (const char *)NULL) {
    // It is defined in the environment; thus, it is implicitly
    // defined here at the global scope: the bottom of the stack.
    PPScope *bottom = this;
    if (!_scope_stack.empty()) {
      bottom = _scope_stack.front();
    }
    bottom->define_variable(varname, definition);
    return true;
  }

  // The variable isn't defined anywhere.  Too bad.
  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::define_map_variable
//       Access: Public
//  Description: Makes a new map variable definition.  This defines a
//               new variable that can be used as a function to
//               retrieve variables from within a named scope, based
//               on a particular key variable.
//
//               In this variant of define_map_variable(), the
//               definition is a string of the form
//               key_varname(scope_names).
////////////////////////////////////////////////////////////////////
void PPScope::
define_map_variable(const string &varname, const string &definition) {
  size_t p = definition.find(VARIABLE_OPEN_NESTED);
  if (p != string::npos && definition[definition.length() - 1] == VARIABLE_CLOSE_NESTED) {
    size_t q = definition.length() - 1;
    string scope_names = definition.substr(p + 1, q - (p + 1));
    string key_varname = definition.substr(0, p);
    define_map_variable(varname, key_varname, scope_names);
  } else {
    // No scoping; not really a map variable.
    define_map_variable(varname, definition, "");
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::define_map_variable
//       Access: Public
//  Description: Makes a new map variable definition.  This defines a
//               new variable that can be used as a function to
//               retrieve variables from within a named scope, based
//               on a particular key variable.
////////////////////////////////////////////////////////////////////
void PPScope::
define_map_variable(const string &varname, const string &key_varname,
		    const string &scope_names) {
  MapVariableDefinition &def = _map_variables[varname];
  def.clear();
  define_variable(varname, "");

  if (_named_scopes == (PPNamedScopes *)NULL) {
    return;
  }

  if (key_varname.empty()) {
    return;
  }

  vector<string> names;
  tokenize_whitespace(scope_names, names);

  // Get all of the named scopes.
  PPNamedScopes::Scopes scopes;
  
  vector<string>::const_iterator ni;
  for (ni = names.begin(); ni != names.end(); ++ni) {
    const string &name = (*ni);
    _named_scopes->get_scopes(name, scopes);
  }

  if (scopes.empty()) {
    return;
  }

  // Now go through the scopes and build up the results.
  vector<string> results;

  PPNamedScopes::Scopes::const_iterator si;
  for (si = scopes.begin(); si != scopes.end(); ++si) {
    PPScope *scope = (*si);
    string key_string = scope->expand_variable(key_varname);
    vector<string> keys;
    tokenize_whitespace(key_string, keys);

    if (!keys.empty()) {
      vector<string>::const_iterator ki;
      results.insert(results.end(), keys.begin(), keys.end());
      for (ki = keys.begin(); ki != keys.end(); ++ki) {
	def[*ki] = scope;
      }
    }
  }

  // Also define a traditional variable along with the map variable.
  define_variable(varname, repaste(results, " "));
}


////////////////////////////////////////////////////////////////////
//     Function: PPScope::add_to_map_variable
//       Access: Public
//  Description: Adds a new key/scope pair to a previous map variable
//               definition.
////////////////////////////////////////////////////////////////////
void PPScope::
add_to_map_variable(const string &varname, const string &key,
		    PPScope *scope) {
  MapVariableDefinition &def = find_map_variable(varname);
  if (&def == &_null_map_def) {
    cerr << "Warning:  undefined map variable: " << varname << "\n";
    return;
  }

  def[key] = scope;

  // We need to do all this work to define the traditional expansion.
  // Maybe not a great idea.
  vector<string> results;
  MapVariableDefinition::const_iterator di;
  for (di = def.begin(); di != def.end(); ++di) {
    results.push_back((*di).first);
  }

  set_variable(varname, repaste(results, " "));
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::define_formals
//       Access: Public
//  Description: Supplies values to a slew of variables at once,
//               typically to define actual values for a list of
//               formal parameters to a user-defined subroutine or
//               function.
//
//               Formals is a vector of variable names to be defined,
//               and actuals is a comma-separated list of expressions
//               to be substituted in, one-per-one.  The
//               subroutine_name is used only for error reporting.
////////////////////////////////////////////////////////////////////
void PPScope::
define_formals(const string &subroutine_name, 
	       const vector<string> &formals, const string &actuals) {
  vector<string> actual_words;
  tokenize_params(actuals, actual_words, true);

  if (actual_words.size() < formals.size()) {
    cerr << "Warning: not all parameters defined for " << subroutine_name
	 << ": " << actuals << "\n";
  } else if (actual_words.size() > formals.size()) {
    cerr << "Warning: more parameters defined for " << subroutine_name
	 << " than actually exist: " << actuals << "\n";
  }

  for (int i = 0; i < (int)formals.size(); i++) {
    if (i < (int)actual_words.size()) {
      define_variable(formals[i], actual_words[i]);
    } else {
      define_variable(formals[i], string());
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::get_variable
//       Access: Public
//  Description: Returns the variable definition associated with the
//               indicated variable name.
////////////////////////////////////////////////////////////////////
string PPScope::
get_variable(const string &varname) const {
  // Is it a user-defined function?
  const PPSubroutine *sub = PPSubroutine::get_func(varname);
  if (sub != (const PPSubroutine *)NULL) {
    return expand_function(varname, sub, string());
  }      

  string result;
  if (p_get_variable(varname, result)) {
    return result;
  }

  // Check the scopes on the stack for the variable definition.
  ScopeStack::reverse_iterator si;
  for (si = _scope_stack.rbegin(); si != _scope_stack.rend(); ++si) {
    if ((*si)->p_get_variable(varname, result)) {
      return result;
    }
  }

  // If the variable isn't defined, we check the environment.
  const char *env = getenv(varname.c_str());
  if (env != (const char *)NULL) {
    return env;
  }

  // It's not defined anywhere, so it's implicitly empty.
  return string();
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_variable
//       Access: Public
//  Description: Similar to get_variable(), except the variable
//               definition is in turn expanded.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_variable(const string &varname) const {
  return expand_string(get_variable(varname));
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::find_map_variable
//       Access: Public
//  Description: Looks for the map variable definition in this scope
//               or some ancestor scope.  Returns the map variable
//               definition if it is found, or _null_map_def if it is
//               not.
////////////////////////////////////////////////////////////////////
PPScope::MapVariableDefinition &PPScope::
find_map_variable(const string &varname) const {
  MapVariableDefinition &def = p_find_map_variable(varname);
  if (&def != &_null_map_def) {
    return def;
  }

  // No such map variable.  Check the stack.
  ScopeStack::reverse_iterator si;
  for (si = _scope_stack.rbegin(); si != _scope_stack.rend(); ++si) {
    MapVariableDefinition &def = (*si)->p_find_map_variable(varname);
    if (&def != &_null_map_def) {
      return def;
    }
  }

  // Nada.
  return _null_map_def;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::get_directory
//       Access: Public
//  Description: Returns the directory level associated with this
//               scope, if any, or with the nearest parent to this
//               scope.
////////////////////////////////////////////////////////////////////
PPDirectoryTree *PPScope::
get_directory() const {
  if (_directory != (PPDirectoryTree *)NULL) {
    return _directory;
  }

  // Check the stack.
  ScopeStack::reverse_iterator si;
  for (si = _scope_stack.rbegin(); si != _scope_stack.rend(); ++si) {
    if ((*si)->_directory != (PPDirectoryTree *)NULL) {
      return (*si)->_directory;
    }
  }

  return (PPDirectoryTree *)NULL;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::set_directory
//       Access: Public
//  Description: Associates this scope with the indicated directory
//               level.  Typically this is done when definition a
//               scope for a particular source file which exists at a
//               known directory level.
////////////////////////////////////////////////////////////////////
void PPScope::
set_directory(PPDirectoryTree *directory) {
  _directory = directory;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_string
//       Access: Public
//  Description: Expands out all the variable references in the given
//               string.  Variables are expanded recursively; that is,
//               if a variable expansion includes a reference to
//               another variable name, the second variable name is
//               expanded.  However, cyclical references are not
//               expanded.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_string(const string &str) const {
  return r_expand_string(str, (ExpandedVariable *)NULL);
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_self_reference
//       Access: Public
//  Description: Similar to expand_string(), except that only simple
//               references to the named variable are expanded--other
//               variable references are left unchanged.  This allows
//               us to define a variable in terms of its previous
//               definition.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_self_reference(const string &str, const string &varname) const {
  // Look for a simple reference to the named variable.  A more
  // complex reference, like a computed variable name or something
  // equally loopy, won't work with this simple test.  Too bad.
  string reference;
  reference += VARIABLE_PREFIX;
  reference += VARIABLE_OPEN_BRACE;
  reference += varname;
  reference += VARIABLE_CLOSE_BRACE;

  string result;

  size_t p = 0;
  size_t q = str.find(reference, p);
  while (q != string::npos) {
    result += str.substr(p, q - p);
    p = q;
    result += r_expand_variable(str, p, (ExpandedVariable *)NULL);
    q = str.find(reference, p);  
  }

  result += str.substr(p);
  return result;
}


////////////////////////////////////////////////////////////////////
//     Function: PPScope::push_scope
//       Access: Public, Static
//  Description: Pushes the indicated scope onto the top of the stack.
//               When a variable reference is unresolved in the
//               current scope, the scope stack is searched, in LIFO
//               order.
////////////////////////////////////////////////////////////////////
void PPScope::
push_scope(PPScope *scope) {
  _scope_stack.push_back(scope);
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::pop_scope
//       Access: Public, Static
//  Description: Pops another level off the top of the stack.  See
//               push_scope().
////////////////////////////////////////////////////////////////////
PPScope *PPScope::
pop_scope() {
  assert(!_scope_stack.empty());
  PPScope *back = _scope_stack.back();
  _scope_stack.pop_back();
  return back;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::get_bottom_scope
//       Access: Public, Static
//  Description: Returns the scope on the bottom of the stack.  This
//               was the very first scope ever pushed, e.g. the global
//               scope.
////////////////////////////////////////////////////////////////////
PPScope *PPScope::
get_bottom_scope() {
  assert(!_scope_stack.empty());
  return _scope_stack.front();
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::tokenize_params
//       Access: Public
//  Description: Separates a string into tokens based on comma
//               delimiters, e.g. for parameters to a function.
//               Nested variable references are skipped correctly,
//               even if they include commas.  Leading and trailing
//               whitespace in each token is automatically stripped.
//
//               If expand is true, the nested variables are
//               automatically expanded as the string is tokenized;
//               otherwise, they are left unexpanded.
////////////////////////////////////////////////////////////////////
void PPScope::
tokenize_params(const string &str, vector<string> &tokens,
		bool expand) const {
  size_t p = 0;
  while (p < str.length()) {
    // Skip initial whitespace.
    while (p < str.length() && isspace(str[p])) {
      p++;
    }

    string token;
    while (p < str.length() && str[p] != FUNCTION_PARAMETER_SEPARATOR) {
      if (p + 1 < str.length() && str[p] == VARIABLE_PREFIX &&
	  str[p + 1] == VARIABLE_OPEN_BRACE) {
	// Skip a nested variable reference.
	if (expand) {
	  token += r_expand_variable(str, p, (ExpandedVariable *)NULL);
	} else {
	  token += r_scan_variable(str, p);
	}
      } else {
	token += str[p];
	p++;
      }
    }

    // Back up past trailing whitespace.
    size_t q = token.length();
    while (q > 0 && isspace(token[q - 1])) {
      q--;
    }

    tokens.push_back(token.substr(0, q));
    p++;

    if (p == str.length()) {
      // In this case, we have just read past a trailing comma symbol
      // at the end of the string, so we have one more empty token.
      tokens.push_back(string());
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::p_set_variable
//       Access: Private
//  Description: The private implementation of p_set_variable.
//               Returns true if the variable's definition is found
//               and set, false otherwise.
////////////////////////////////////////////////////////////////////
bool PPScope::
p_set_variable(const string &varname, const string &definition) {
  Variables::iterator vi;
  vi = _variables.find(varname);
  if (vi != _variables.end()) {
    (*vi).second = definition;
    return true;
  }

  if (_parent_scope != (PPScope *)NULL) {
    return _parent_scope->p_set_variable(varname, definition);
  }

  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::p_get_variable
//       Access: Private
//  Description: The private implementation of get_variable().  This
//               checks the local scope only; it does not check the
//               stack.  It returns true if the variable is defined,
//               false otherwise..
////////////////////////////////////////////////////////////////////
bool PPScope::
p_get_variable(const string &varname, string &result) const {
  Variables::const_iterator vi;
  vi = _variables.find(varname);
  if (vi != _variables.end()) {
    result = (*vi).second;
    return true;
  }

  if (varname == "RELDIR" && 
      _directory != (PPDirectoryTree *)NULL &&
      current_output_directory != (PPDirectoryTree *)NULL) {
    // $[RELDIR] is a special variable name that evaluates to the
    // relative directory of the current scope to the current output
    // directory.
    result = current_output_directory->get_rel_to(_directory);
    return true;
  }

  if (varname == "DEPENDS_INDEX" && 
      _directory != (PPDirectoryTree *)NULL) {
    // $[DEPENDS_INDEX] is another special variable name that
    // evaluates to the numeric sorting index assigned to this
    // directory based on its dependency relationship with other
    // directories.  It's useful primarily for debugging.
    char buffer[32];
    sprintf(buffer, "%d", _directory->get_depends_index());
    result = buffer;
    return true;
  }

  if (_parent_scope != (PPScope *)NULL) {
    return _parent_scope->p_get_variable(varname, result);
  }

  return false;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::r_expand_string
//       Access: Private
//  Description: The recursive implementation of expand_string().
//               This function detects cycles in the variable
//               expansion by storing the set of variable names that
//               have thus far been expanded in the linked list.
////////////////////////////////////////////////////////////////////
string PPScope::
r_expand_string(const string &str, PPScope::ExpandedVariable *expanded) const {
  string result;

  // Search for a variable reference.
  size_t p = 0;
  while (p < str.length()) {
    if (p + 1 < str.length() && str[p] == VARIABLE_PREFIX &&
	str[p + 1] == VARIABLE_OPEN_BRACE) {
      // Here's a nested variable!  Expand it fully.
      result += r_expand_variable(str, p, expanded);

    } else {
      result += str[p];
      p++;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::r_scan_variable
//       Access: Private
//  Description: Scans past a single variable reference without
//               expanding it.  On input, str is a string containing a
//               variable reference (among other stuff), and vp is the
//               position within the string of the prefix character at
//               the beginning of the variable reference.
//
//               On output, vp is set to the position within the
//               string of the first character after the variable
//               reference's closing bracket.  The variable reference
//               itself is returned.
////////////////////////////////////////////////////////////////////
string PPScope::
r_scan_variable(const string &str, size_t &vp) const {

  // Search for the end of the variable name: an unmatched square
  // bracket.
  size_t start = vp;
  size_t p = vp + 2;
  while (p < str.length() && str[p] != VARIABLE_CLOSE_BRACE) {
    if (p + 1 < str.length() && str[p] == VARIABLE_PREFIX && 
	str[p + 1] == VARIABLE_OPEN_BRACE) {
      // Here's a nested variable!  Scan past it, matching braces
      // properly.
      r_scan_variable(str, p);
    } else {
      p++;
    }
  }

  if (p < str.length()) {
    assert(str[p] == VARIABLE_CLOSE_BRACE);
    p++;
  } else {
    cerr << "Warning!  Unclosed variable reference:\n"
	 << str.substr(vp) << "\n";
  }

  vp = p;
  return str.substr(start, vp - start);
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::r_expand_variable
//       Access: Private
//  Description: Expands a single variable reference.  On input, str
//               is a string containing a variable reference (among
//               other stuff), and vp is the position within the
//               string of the prefix character at the beginning of
//               the variable reference.
//
//               On output, vp is set to the position within the
//               string of the first character after the variable
//               reference's closing bracket, and the string expansion
//               of the variable reference is returned.
////////////////////////////////////////////////////////////////////
string PPScope::
r_expand_variable(const string &str, size_t &vp,
		  PPScope::ExpandedVariable *expanded) const {
  string varname;

  size_t whitespace_at = 0;
  size_t open_nested_at = 0;

  // Search for the end of the variable name: an unmatched square
  // bracket.
  size_t p = vp + 2;
  while (p < str.length() && str[p] != VARIABLE_CLOSE_BRACE) {
    if (p + 1 < str.length() && str[p] == VARIABLE_PREFIX && 
	str[p + 1] == VARIABLE_OPEN_BRACE) {
      if (whitespace_at != 0) {
	// Once we have encountered whitespace, we don't expand
	// variables inline anymore.  These are now function
	// parameters, and might need to be expanded in some other
	// scope.
	varname += r_scan_variable(str, p);
      } else {
	varname += r_expand_variable(str, p, expanded);
      }

    } else {
      if (open_nested_at == 0 && str[p] == VARIABLE_OPEN_NESTED) {
	open_nested_at = p - (vp + 2);
      }
      if (open_nested_at == 0 && whitespace_at == 0 && isspace(str[p])) {
	whitespace_at = p - (vp + 2);
      }
      varname += str[p];
      p++;
    }
  }

  if (p < str.length()) {
    assert(str[p] == VARIABLE_CLOSE_BRACE);
    p++;
  } else {
    cerr << "Warning!  Unclosed variable reference:\n"
	 << str.substr(vp) << "\n";
  }

  vp = p;

  // Check for a function expansion.
  if (whitespace_at != 0) {
    string funcname = varname.substr(0, whitespace_at);
    p = whitespace_at;
    while (p < varname.length() && isspace(varname[p])) {
      p++;
    }
    string params = varname.substr(p);

    // Is it a user-defined function?
    const PPSubroutine *sub = PPSubroutine::get_func(funcname);
    if (sub != (const PPSubroutine *)NULL) {
      return expand_function(funcname, sub, params);
    }      

    // Is it a built-in function?
    if (funcname == "wildcard") {
      return expand_wildcard(params);
    } else if (funcname == "isdir") {
      return expand_isdir(params);
    } else if (funcname == "isfile") {
      return expand_isfile(params);
    } else if (funcname == "libtest") {
      return expand_libtest(params);
    } else if (funcname == "bintest") {
      return expand_bintest(params);
    } else if (funcname == "shell") {
      return expand_shell(params);
    } else if (funcname == "standardize") {
      return expand_standardize(params);
    } else if (funcname == "firstword") {
      return expand_firstword(params);
    } else if (funcname == "patsubst") {
      return expand_patsubst(params);
    } else if (funcname == "subst") {
      return expand_subst(params);
    } else if (funcname == "filter") {
      return expand_filter(params);
    } else if (funcname == "filter_out" || funcname == "filter-out") {
      return expand_filter_out(params);
    } else if (funcname == "sort") {
      return expand_sort(params);
    } else if (funcname == "unique") {
      return expand_unique(params);
    } else if (funcname == "if") {
      return expand_if(params);
    } else if (funcname == "eq") {
      return expand_eq(params);
    } else if (funcname == "ne") {
      return expand_ne(params);
    } else if (funcname == "not") {
      return expand_not(params);
    } else if (funcname == "or") {
      return expand_or(params);
    } else if (funcname == "and") {
      return expand_and(params);
    } else if (funcname == "upcase") {
      return expand_upcase(params);
    } else if (funcname == "downcase") {
      return expand_downcase(params);
    } else if (funcname == "cdefine") {
      return expand_cdefine(params);
    } else if (funcname == "closure") {
      return expand_closure(params);
    } else if (funcname == "unmapped") {
      return expand_unmapped(params);
    }

    // It must be a map variable.
    return expand_map_variable(funcname, params);
  }

  // Now we have the variable name; was it previously expanded?
  ExpandedVariable *ev;
  for (ev = expanded; ev != (ExpandedVariable *)NULL; ev = ev->_next) {
    if (ev->_varname == varname) {
      // Yes, this is a cyclical expansion.
      cerr << "Ignoring cyclical expansion of " << varname << "\n";
      return string();
    }
  }

  // And now expand the variable.

  string expansion;

  // Check for a special inline patsubst operation, like GNU make:
  // $[varname:%.c=%.o]
  string patsubst;
  bool got_patsubst = false;
  p = varname.find(variable_patsubst);
  if (p != string::npos) {
    got_patsubst = true;
    patsubst = varname.substr(p + variable_patsubst.length());
    varname = varname.substr(0, p);
  }

  // Check for special scoping operators in the variable name.
  p = varname.find(VARIABLE_OPEN_NESTED);
  if (p != string::npos && varname[varname.length() - 1] == VARIABLE_CLOSE_NESTED) {
    size_t q = varname.length() - 1;
    string scope_names = varname.substr(p + 1, q - (p + 1));
    varname = varname.substr(0, p);
    expansion = expand_variable_nested(varname, scope_names);

  } else {
    // No special scoping; just expand the variable name.
    expansion = get_variable(varname);
  }

  // Finally, recursively expand any variable references in the
  // variable's expansion.
  ExpandedVariable new_var;
  new_var._varname = varname;
  new_var._next = expanded;
  string result = r_expand_string(expansion, &new_var);

  // And *then* apply any inline patsubst.
  if (got_patsubst) {
    vector<string> tokens;
    tokenize(patsubst, tokens, VARIABLE_PATSUBST_DELIM);
    
    if (tokens.size() != 2) {
      cerr << "inline patsubst should be of the form "
	   << VARIABLE_PREFIX << VARIABLE_OPEN_BRACE << "varname"
	   << VARIABLE_PATSUBST << PATTERN_WILDCARD << ".c"
	   << VARIABLE_PATSUBST_DELIM << PATTERN_WILDCARD << ".o"
	   << VARIABLE_CLOSE_BRACE << ".\n";
    } else {
      PPFilenamePattern from(tokens[0]);
      PPFilenamePattern to(tokens[1]);
    
      if (!from.has_wildcard() || !to.has_wildcard()) {
	cerr << "The two parameters of inline patsubst must both include "
	     << PATTERN_WILDCARD << ".\n";
	return string();
      }
    
      // Split the expansion into tokens based on the spaces.
      vector<string> words;
      tokenize_whitespace(result, words);
      
      vector<string>::iterator wi;
      for (wi = words.begin(); wi != words.end(); ++wi) {
	(*wi) = to.transform(*wi, from);
      }
    
      result = repaste(words, " ");
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_variable_nested
//       Access: Private
//  Description: Expands a variable reference of the form
//               $[varname(scope scope scope)].  This means to
//               concatenate the expansions of the variable in all of
//               the named scopes.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_variable_nested(const string &varname, 
		       const string &scope_names) const {
  if (_named_scopes == (PPNamedScopes *)NULL) {
    return string();
  }

  vector<string> names;
  tokenize_whitespace(scope_names, names);

  // Get all of the named scopes.
  PPNamedScopes::Scopes scopes;
  
  vector<string>::const_iterator ni;
  for (ni = names.begin(); ni != names.end(); ++ni) {
    const string &name = (*ni);
    _named_scopes->get_scopes(name, scopes);
  }

  if (scopes.empty()) {
    return string();
  }

  // Now go through the scopes and build up the results.
  vector<string> results;

  PPNamedScopes::Scopes::const_iterator si;
  for (si = scopes.begin(); si != scopes.end(); ++si) {
    PPScope *scope = (*si);
    string nested = scope->expand_variable(varname);
    if (!nested.empty()) {
      results.push_back(nested);
    }
  }

  string result = repaste(results, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_wildcard
//       Access: Private
//  Description: Expands the "wildcard" function variable.  This
//               returns the set of files matched by the parameters
//               with shell matching characters.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_wildcard(const string &params) const {
  vector<string> results;
  glob_string(expand_string(params), results);

  string result = repaste(results, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_isdir
//       Access: Private
//  Description: Expands the "isdir" function variable.  This
//               returns true if the parameter exists and is a
//               directory, or false otherwise.  This actually expands
//               the parameter(s) with shell globbing characters,
//               similar to the "wildcard" function, and looks only at
//               the first expansion.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_isdir(const string &params) const {
  vector<string> results;
  glob_string(expand_string(params), results);

  if (results.empty()) {
    // No matching file, too bad.
    return string();
  }

  const string &filename = results[0];
  struct stat stbuf;

  string result;
  if (stat(filename.c_str(), &stbuf) == 0) {
    if (S_ISDIR(stbuf.st_mode)) {
      result = filename;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_isfile
//       Access: Private
//  Description: Expands the "isfile" function variable.  This
//               returns true if the parameter exists and is a
//               regular file, or false otherwise.  This actually
//               expands the parameter(s) with shell globbing
//               characters, similar to the "wildcard" function, and
//               looks only at the first expansion.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_isfile(const string &params) const {
  vector<string> results;
  glob_string(expand_string(params), results);

  if (results.empty()) {
    // No matching file, too bad.
    return string();
  }

  const string &filename = results[0];
  struct stat stbuf;

  string result;
  if (stat(filename.c_str(), &stbuf) == 0) {
    if (S_ISREG(stbuf.st_mode)) {
      result = filename;
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_libtest
//       Access: Private
//  Description: Expands the "libtest" function variable.  This
//               serves as a poor man's autoconf feature to check to
//               see if a library by the given name exists on the
//               indicated search path, or on the system search path.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_libtest(const string &params) const {
  // Get the parameters out based on commas.  The first parameter is a
  // space-separated set of directories to search, the second
  // parameter is a space-separated set of library names.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 2) {
    cerr << "libtest requires two parameters.\n";
    return string();
  }

  vector<string> directories;
  tokenize_whitespace(tokens[0], directories);

  // Also add the system directories to the list, whatever we think
  // those should be.  Here we have to make a few assumptions.
#ifdef PLATFORM_WIN32
  const char *windir = getenv("WINDIR");
  if (windir != (const char *)NULL) {
    directories.push_back(string(windir) + "\\System");
    directories.push_back(string(windir) + "\\System32");
  }

  const char *lib = getenv("LIB");
  if (lib != (const char *)NULL) {
    tokenize(lib, directories, ";");
  }
#endif

  // We'll also check the Unix standard places, even if we're building
  // Windows, since we might be using Cygwin.

  // Check LD_LIBRARY_PATH.
  const char *ld_library_path = getenv("LD_LIBRARY_PATH");
  if (ld_library_path != (const char *)NULL) {
    tokenize(ld_library_path, directories, ":");
  }

  directories.push_back("/lib");
  directories.push_back("/usr/lib");

  vector<string> libnames;
  tokenize_whitespace(tokens[1], libnames);

  if (libnames.empty()) {
    // No libraries is a default "false".
    return string();
  }

  // We only bother to search for the first library name in the list.
  string libname = libnames[0];

  string found;

#ifdef PLATFORM_WIN32
  if (libname.length() > 4 && libname.substr(libname.length() - 4) == ".lib") {
    found = find_searchpath(directories, libname);    
    if (found.empty()) {
      found = find_searchpath(directories, libname.substr(0, libname.length() - 4) + ".dll");
    }
  } else {
    found = find_searchpath(directories, "lib" + libname + ".lib");
    if (found.empty()) {
      found = find_searchpath(directories, "lib" + libname + ".dll");
    }
  }
  
#else
  found = find_searchpath(directories, "lib" + libname + ".a");
  if (found.empty()) {
    found = find_searchpath(directories, "lib" + libname + ".so");
  }
#endif


  return found;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_bintest
//       Access: Private
//  Description: Expands the "bintest" function variable.  This
//               serves as a poor man's autoconf feature to check to
//               see if an executable program by the given name exists
//               on the indicated search path, or on the system search
//               path.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_bintest(const string &params) const {
  // We only have one parameter: the filename of the executable.  We
  // always search for it on the path.
  string binname = expand_string(params);

  if (binname.empty()) {
    // No binary, no exist.
    return string();
  }

  // An explicit path from the root does not require a search.
#ifdef PLATFORM_WIN32
  if ((binname.length() > 2 && binname[1] == ':') || binname[0] == '/')
#else
  if (binname[0] == '/')
#endif
    {
    if (access(binname.c_str(), F_OK) == 0) {
      return binname;
    }
    return string();
  }

  const char *path = getenv("PATH");
  if (path == (const char *)NULL) {
    // If the path is undefined, too bad.
    return string();
  }

  string pathvar(path);

  vector<string> directories;

#ifdef PLATFORM_WIN32
  if (pathvar.find(';') != string::npos) {
    // If the path contains semicolons, it's a native Windows-style
    // path: split it up based on semicolons.
    tokenize(pathvar, directories, ";");

  } else {
    // Otherwise, assume it's a Cygwin-style path: split it up based
    // on colons.
    tokenize(pathvar, directories, ":");
  }
#else
  tokenize(pathvar, directories, ":");
#endif

  string found;

#ifdef PLATFORM_WIN32
  found = find_searchpath(directories, binname + ".exe");
  if (found.empty()) {
    found = find_searchpath(directories, binname);
  }
  
#else
  found = find_searchpath(directories, binname);
#endif

  return found;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_shell
//       Access: Private
//  Description: Expands the "shell" function variable.  This executes
//               the given command in a subprocess and returns the
//               standard output.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_shell(const string &params) const {
  string command = expand_string(params);
  int pid, status;

  int pd[2];
  if (pipe(pd) < 0) {
    // pipe() failed.
    perror("pipe");
    return string();
  }

  pid = fork();
  if (pid < 0) {
    // fork() failed.
    perror("fork");
    return string();
  }
    
  if (pid == 0) {
    // Child.
    dup2(pd[1], STDOUT_FILENO);
    char *argv[4];
    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = (char *)command.c_str();
    argv[3] = (char *)NULL;
    execv("/bin/sh", argv);
    exit(127);
  }

  // Parent.  Wait for the child to terminate, and read from its
  // output while we're waiting.
  close(pd[1]);
  bool child_done = false;
  bool pipe_closed = false;
  string output;

  while (!child_done && !pipe_closed) {
    static const int buffer_size = 1024;
    char buffer[buffer_size];
    int read_bytes = (int)read(pd[0], buffer, buffer_size);
    if (read_bytes < 0) {
      perror("read");
    } else if (read_bytes == 0) {
      pipe_closed = true;
    } else {
      output += string(buffer, read_bytes);
    }

    if (!child_done) {
      int waitresult = waitpid(pid, &status, WNOHANG);
      if (waitresult < 0) {
	if (errno != EINTR) {
	  perror("waitpid");
	  return string();
	}
      } else if (waitresult > 0) {
	child_done = true;
      }
    }
  }
  close(pd[0]);

  // Now get the output.  We split it into words and then reconnect
  // it, to simulate the shell's backpop operator.
  vector<string> results;
  tokenize_whitespace(output, results);

  string result = repaste(results, " ");

  return result;
}


////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_standardize
//       Access: Private
//  Description: Expands the "standardize" function variable.  This
//               converts the filename to standard form by removing
//               consecutive repeated slashes and collapsing /../
//               where possible.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_standardize(const string &params) const {
  string filename = expand_string(params);
  if (filename.empty()) {
    return string();
  }

  vector<string> components;

  // Pull off the components of the filename one at a time.
  bool global = (filename[0] == '/');

  size_t p = 0;
  while (p < filename.length() && filename[p] == '/') {
    p++;
  }
  while (p < filename.length()) {
    size_t slash = filename.find('/', p);
    string component = filename.substr(p, slash - p);
    if (component == ".") {
      // Ignore /./.
    } else if (component == ".." && !components.empty() && 
	       !(components.back() == "..")) {
      // Back up.
      components.pop_back();
    } else {
      components.push_back(component);
    }

    p = slash;
    while (p < filename.length() && filename[p] == '/') {
      p++;
    }
  }
   
  // Now reassemble the filename.
  string result;
  if (global) {
    result = "/";
  }
  if (!components.empty()) {
    result += components[0];
    for (int i = 1; i < (int)components.size(); i++) {
      result += "/" + components[i];
    }
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_firstword
//       Access: Private
//  Description: Expands the "firstword" function variable.  This
//               returns the first of several words separated by
//               whitespace.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_firstword(const string &params) const {
  // Split the parameter into tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(expand_string(params), words);

  if (!words.empty()) {
    return words[0];
  }
  return string();
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_patsubst
//       Access: Private
//  Description: Expands the "patsubst" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_patsubst(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() < 3) {
    cerr << "patsubst requires at least three parameters.\n";
    return string();
  }

  if ((tokens.size() % 2) != 1) {
    cerr << "subst requires an odd number of parameters.\n";
    return string();
  }

  // Split the last parameter into tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(tokens.back(), words);

  // Build up a vector of from/to patterns.
  typedef vector<PPFilenamePattern> Patterns;
  typedef vector<Patterns> FromPatterns;
  FromPatterns from;
  Patterns to;

  size_t i;
  for (i = 0; i < tokens.size() - 1; i += 2) {
    // Each "from" pattern might be a collection of patterns separated
    // by spaces.
    from.push_back(Patterns());
    vector<string> froms;
    tokenize_whitespace(tokens[i], froms);
    vector<string>::const_iterator fi;
    for (fi = froms.begin(); fi != froms.end(); ++fi) {
      PPFilenamePattern pattern(*fi);
      if (!pattern.has_wildcard()) {
	cerr << "All the \"from\" parameters of patsubst must include "
	     << PATTERN_WILDCARD << ".\n";
	return string();
      }
      from.back().push_back(pattern);
    }

    // However, the corresponding "to" pattern is just one pattern.
    to.push_back(PPFilenamePattern(tokens[i + 1]));
  }
  size_t num_patterns = from.size();
  assert(num_patterns == to.size());
  
  vector<string>::iterator wi;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    bool matched = false;
    for (i = 0; i < num_patterns && !matched; i++) {
      Patterns::const_iterator pi;
      for (pi = from[i].begin(); pi != from[i].end() && !matched; ++pi) {
	if ((*pi).matches(*wi)) {
	  matched = true;
	  (*wi) = to[i].transform(*wi, (*pi));
	}
      }
    }
  }

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_filter
//       Access: Private
//  Description: Expands the "filter" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_filter(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 2) {
    cerr << "filter requires two parameters.\n";
    return string();
  }

  // Split up the first parameter--the list of patterns to filter
  // by--into tokens based on the spaces.
  vector<string> pattern_strings;
  tokenize_whitespace(tokens[0], pattern_strings);

  vector<PPFilenamePattern> patterns;
  vector<string>::const_iterator psi;
  for (psi = pattern_strings.begin(); psi != pattern_strings.end(); ++psi) {
    patterns.push_back(PPFilenamePattern(*psi));
  }

  // Split up the second parameter--the list of words to filter--into
  // tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(tokens[1], words);

  vector<string>::iterator wi, wnext;
  wnext = words.begin();
  for (wi = words.begin(); wi != words.end(); ++wi) {
    const string &word = (*wi);

    bool matches_pattern = false;
    vector<PPFilenamePattern>::const_iterator pi;
    for (pi = patterns.begin(); 
	 pi != patterns.end() && !matches_pattern; 
	 ++pi) {
      matches_pattern = (*pi).matches(word);
    }

    if (matches_pattern) {
      *wnext++ = word;
    }
  }

  words.erase(wnext, words.end());

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_filter_out
//       Access: Private
//  Description: Expands the "filter_out" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_filter_out(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 2) {
    cerr << "filter requires two parameters.\n";
    return string();
  }

  // Split up the first parameter--the list of patterns to filter
  // by--into tokens based on the spaces.
  vector<string> pattern_strings;
  tokenize_whitespace(tokens[0], pattern_strings);

  vector<PPFilenamePattern> patterns;
  vector<string>::const_iterator psi;
  for (psi = pattern_strings.begin(); psi != pattern_strings.end(); ++psi) {
    patterns.push_back(PPFilenamePattern(*psi));
  }

  // Split up the second parameter--the list of words to filter--into
  // tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(tokens[1], words);

  vector<string>::iterator wi, wnext;
  wnext = words.begin();
  for (wi = words.begin(); wi != words.end(); ++wi) {
    const string &word = (*wi);

    bool matches_pattern = false;
    vector<PPFilenamePattern>::const_iterator pi;
    for (pi = patterns.begin(); 
	 pi != patterns.end() && !matches_pattern; 
	 ++pi) {
      matches_pattern = (*pi).matches(word);
    }

    if (!matches_pattern) {
      *wnext++ = word;
    }
  }

  words.erase(wnext, words.end());

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_subst
//       Access: Private
//  Description: Expands the "subst" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_subst(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() < 3) {
    cerr << "subst requires at least three parameters.\n";
    return string();
  }

  if ((tokens.size() % 2) != 1) {
    cerr << "subst requires an odd number of parameters.\n";
    return string();
  }

  // Split the last parameter into tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(tokens.back(), words);
  
  vector<string>::iterator wi;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    string &word = (*wi);

    // Check for the given word in the subst/replace strings.
    bool found = false;
    for (size_t i = 0; i < tokens.size() - 1 && !found; i += 2) {
      if (tokens[i] == word) {
	found = true;
	word = tokens[i + 1];
      }
    }
  }

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_sort
//       Access: Private
//  Description: Expands the "sort" function variable: sort the words
//               into alphabetical order, and also remove duplicates.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_sort(const string &params) const {
  // Split the string up into tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(expand_string(params), words);

  sort(words.begin(), words.end());
  words.erase(unique(words.begin(), words.end()), words.end());

  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_unique
//       Access: Private
//  Description: Expands the "unique" function variable: remove
//               duplicates from the list of words without changing
//               the order.  The first appearance of each word
//               remains.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_unique(const string &params) const {
  // Split the string up into tokens based on the spaces.
  vector<string> words;
  tokenize_whitespace(expand_string(params), words);

  vector<string>::iterator win, wout;
  set<string> included_words;

  win = words.begin();
  wout = words.begin();
  while (win != words.end()) {
    if (included_words.insert(*win).second) {
      // This is a unique word so far.
      *wout++ = *win;
    }
    ++win;
  }

  words.erase(wout, words.end());
  string result = repaste(words, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_if
//       Access: Private
//  Description: Expands the "if" function variable.  This evaluates
//               the first parameter and returns the second parameter
//               if the result is true (i.e. nonempty) and the third
//               parameter (if present) if the result is faluse
//               (i.e. empty).
////////////////////////////////////////////////////////////////////
string PPScope::
expand_if(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() == 2) {
    if (!tokens[0].empty()) {
      return tokens[1];
    } else {
      return "";
    }
  } else if (tokens.size() == 3) {
    if (!tokens[0].empty()) {
      return tokens[1];
    } else {
      return tokens[2];
    }
  }

  cerr << "if requires two or three parameters.\n";
  return string();
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_eq
//       Access: Private
//  Description: Expands the "eq" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_eq(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 2) {
    cerr << "eq requires two parameters.\n";
    return string();
  }

  string result;
  if (tokens[0] == tokens[1]) {
    result = "1";
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_ne
//       Access: Private
//  Description: Expands the "ne" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_ne(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 2) {
    cerr << "ne requires two parameters.\n";
    return string();
  }

  string result;
  if (!(tokens[0] == tokens[1])) {
    result = "1";
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_not
//       Access: Private
//  Description: Expands the "not" function variable.  This returns
//               nonempty if its argument is empty, empty if its
//               argument is nonempty.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_not(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  if (tokens.size() != 1) {
    cerr << "not requires two parameters.\n";
    return string();
  }

  string result;
  if (tokens[0].empty()) {
    result = "1";
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_or
//       Access: Private
//  Description: Expands the "or" function variable.  This returns
//               nonempty if any of its arguments are nonempty.
//               Specifically, it returns the first nonempty argument.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_or(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  vector<string>::const_iterator ti;
  for (ti = tokens.begin(); ti != tokens.end(); ++ti) {
    if (!(*ti).empty()) {
      return (*ti);
    }
  }
  return string();
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_and
//       Access: Private
//  Description: Expands the "and" function variable.  This returns
//               nonempty if all of its arguments are nonempty.
//               Specifically, it returns the last argument.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_and(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, true);

  vector<string>::const_iterator ti;
  for (ti = tokens.begin(); ti != tokens.end(); ++ti) {
    if ((*ti).empty()) {
      return string();
    }
  }

  if (tokens.empty()) {
    return "1";
  } else {
    return tokens.back();
  }
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_upcase
//       Access: Private
//  Description: Expands the "upcase" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_upcase(const string &params) const {
  string result = expand_string(params);
  string::iterator si;
  for (si = result.begin(); si != result.end(); ++si) {
    (*si) = toupper(*si);
  }
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_downcase
//       Access: Private
//  Description: Expands the "downcase" function variable.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_downcase(const string &params) const {
  string result = expand_string(params);
  string::iterator si;
  for (si = result.begin(); si != result.end(); ++si) {
    (*si) = tolower(*si);
  }
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_cdefine
//       Access: Private
//  Description: Expands the "cdefine" function variable.  This is a
//               convenience function to output a C-style #define or
//               #undef statement based on the value of the named
//               variable.  If the named string is a variable whose
//               definition is nonempty, this returns "#define varname
//               definition".  Otherwise, it returns "#undef varname".
//               This is particularly useful for building up a
//               config.h file.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_cdefine(const string &params) const {
  string varname = trim_blanks(params);
  string expansion = trim_blanks(expand_variable(varname));

  string result;
  if (expansion.empty()) {
    result = "#undef " + varname;
  } else {
    result = "#define " + varname + " " + expansion;
  }

  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_closure
//       Access: Private
//  Description: Expands the "closure" function variable.  This is a
//               special function that recursively expands a map
//               variable with the given parameter string until all
//               definitions have been encountered.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_closure(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, false);

  if (tokens.size() != 2 && tokens.size() != 3) {
    cerr << "closure requires two or three parameters.\n";
    return string();
  }

  // The first parameter is the map variable name, the second
  // parameter is the expression to evaluate, and the third parameter
  // (if present) is the expression that leads to the recursive
  // evaluation of the map variable.
  string varname = expand_string(tokens[0]);
  string expression = tokens[1];
  string close_on = expression;
  if (tokens.size() > 2) {
    close_on = tokens[2];
  }

  const MapVariableDefinition &def = find_map_variable(varname);
  if (&def == &_null_map_def) {
    cerr << "Warning:  undefined map variable: " << varname << "\n";
    return string();
  }

  // Now evaluate the expression within this scope, and then again
  // within each scope indicated by the result, and then within each
  // scope indicated by *that* result, and so on.  We need to keep
  // track of the words we have already evaluated (hence the set), and
  // we also need to keep track of all the partial results we have yet
  // to evaluate (hence the vector of strings).
  set<string> closure;
  vector<string> results;
  vector<string> next_pass;

  // Start off with the expression evaluated within the starting
  // scope.
  results.push_back(expand_string(expression));

  next_pass.push_back(expand_string(close_on));

  while (!next_pass.empty()) {
    // Pull off one of the partial results (it doesn't matter which
    // one), and chop it up into its constituent words.
    vector<string> pass;
    tokenize_whitespace(next_pass.back(), pass);
    next_pass.pop_back();

    // And then map each of those words into scopes.
    vector<string>::const_iterator wi;
    for (wi = pass.begin(); wi != pass.end(); ++wi) {
      const string &word = (*wi);
      bool inserted = closure.insert(word).second;
      if (inserted) {
	// This is a new word, which presumably maps to a scope.
	MapVariableDefinition::const_iterator di;
	di = def.find(word);
	if (di != def.end()) {
	  PPScope *scope = (*di).second;
	  // Evaluate the expression within this scope.
	  results.push_back(scope->expand_string(expression));
	  
	  // What does close_on evaluate to within this scope?  That
	  // points us to the next scope(s).
	  next_pass.push_back(scope->expand_string(close_on));
	}
      }
    }
  }

  // Now we have the complete transitive closure of $[mapvar close_on].
  string result = repaste(results, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_unmapped
//       Access: Private
//  Description: Expands the "closure" function variable.  This is a
//               special function that returns all the arguments to a
//               map variable, unchanged, that did *not* match any of
//               the keys in the map.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_unmapped(const string &params) const {
  // Split the string up into tokens based on the commas.
  vector<string> tokens;
  tokenize_params(params, tokens, false);

  if (tokens.size() != 2) {
    cerr << "unmapped requires two parameters.\n";
    return string();
  }

  // The first parameter is the map variable name, and the second
  // parameter is the space-separated list of arguments to the map.
  string varname = expand_string(tokens[0]);
  vector<string> keys;
  tokenize_whitespace(expand_string(tokens[1]), keys);

  const MapVariableDefinition &def = find_map_variable(varname);
  if (&def == &_null_map_def) {
    cerr << "Warning:  undefined map variable: " << varname << "\n";
    return string();
  }

  vector<string> results;
  vector<string>::const_iterator ki;
  for (ki = keys.begin(); ki != keys.end(); ++ki) {
    MapVariableDefinition::const_iterator di;
    di = def.find(*ki);
    if (di == def.end()) {
      // This key was undefined.
      results.push_back(*ki);
    }
  }

  string result = repaste(results, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_function
//       Access: Private
//  Description: Expands the user-defined function reference.  This
//               invokes the nested commands within the function body,
//               and returns all the output text as one line.  Quite a
//               job, really.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_function(const string &funcname, 
		const PPSubroutine *sub, const string &params) const {
  PPScope::push_scope((PPScope *)this);
  PPScope nested_scope(_named_scopes);
  nested_scope.define_formals(funcname, sub->_formals, params);

  // This won't compile on VC++.  It has only ostringstream, which is
  // functionally equivalent but has a slightly different interface.
  ostrstream ostr;

  PPCommandFile command(&nested_scope);
  command.set_output(&ostr);

  command.begin_read();
  bool okflag = true;
  vector<string>::const_iterator li;
  for (li = sub->_lines.begin(); li != sub->_lines.end() && okflag; ++li) {
    okflag = command.read_line(*li);
  }
  if (okflag) {
    okflag = command.end_read();
  }

  PPScope::pop_scope();

  // Now get the output.  We split it into words and then reconnect
  // it, to replace all whitespace with spaces.
  ostr << ends;
  char *str = ostr.str();

  vector<string> results;
  tokenize_whitespace(str, results);

  string result = repaste(results, " ");
  delete[] str;
  
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_map_variable
//       Access: Private
//  Description: Expands a map variable function reference.  This
//               looks up the given keys in the map and expands the
//               first parameter for each corresponding scope.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_map_variable(const string &varname, const string &params) const {
  // Split the string up into tokens based on the commas, but don't
  // expand the variables yet.
  vector<string> tokens;
  tokenize_params(params, tokens, false);

  if (tokens.size() != 2) {
    cerr << "map variable expansions require two parameters: $["
	 << varname << " " << params << "]\n";
    return string();
  }

  // Split the second parameter into tokens based on the spaces.  This
  // is the set of keys.
  vector<string> keys;
  tokenize_whitespace(expand_string(tokens[1]), keys);

  return expand_map_variable(varname, tokens[0], keys);
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::expand_map_variable
//       Access: Private
//  Description: Expands a map variable function reference.  This
//               looks up the given keys in the map and expands the
//               expression for each corresponding scope.
////////////////////////////////////////////////////////////////////
string PPScope::
expand_map_variable(const string &varname, const string &expression,
		    const vector<string> &keys) const {
  const MapVariableDefinition &def = find_map_variable(varname);
  if (&def == &_null_map_def) {
    cerr << "Warning:  undefined map variable: " << varname << "\n";
    return string();
  }

  vector<string> results;

  // Now build up the set of expansions of the expression in the
  // various scopes indicated by the keys.
  vector<string>::const_iterator wi;
  for (wi = keys.begin(); wi != keys.end(); ++wi) {
    MapVariableDefinition::const_iterator di;
    di = def.find(*wi);
    if (di != def.end()) {
      PPScope *scope = (*di).second;
      string expansion = scope->expand_string(expression);
      if (!expansion.empty()) {
	results.push_back(expansion);
      }
    }
  }

  string result = repaste(results, " ");
  return result;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::p_find_map_variable
//       Access: Private
//  Description: The implementation of find_map_variable() for a
//               particular static scope, without checking the stack.
////////////////////////////////////////////////////////////////////
PPScope::MapVariableDefinition &PPScope::
p_find_map_variable(const string &varname) const {
  MapVariables::const_iterator mvi;
  mvi = _map_variables.find(varname);
  if (mvi != _map_variables.end()) {
    return (MapVariableDefinition &)(*mvi).second;
  }

  if (_parent_scope != (PPScope *)NULL) {
    return _parent_scope->p_find_map_variable(varname);
  }

  return _null_map_def;
}

////////////////////////////////////////////////////////////////////
//     Function: PPScope::glob_string
//       Access: Private
//  Description: Expands the words in the string as if they were a set
//               of filenames using the shell globbing characters.
//               Fills up the results vector (which the user should
//               ensure is empty before calling) with the set of all
//               files that actually match the globbing characters.
////////////////////////////////////////////////////////////////////
void PPScope::
glob_string(const string &str, vector<string> &results) const {
  vector<string> words;
  tokenize_whitespace(str, words);

  vector<string>::const_iterator wi;

  glob_t pglob;
  memset(&pglob, 0, sizeof(pglob));

  int flags = 0;
  for (wi = words.begin(); wi != words.end(); ++wi) {
    glob((*wi).c_str(), flags, NULL, &pglob);
    flags |= GLOB_APPEND;
  }

  for (int i = 0; i < (int)pglob.gl_pathc; i++) {
    results.push_back(string(pglob.gl_pathv[i]));
  }

  globfree(&pglob);
}