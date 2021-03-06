// imports.cc -- D frontend for GCC.
// Copyright (C) 2016 Free Software Foundation, Inc.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.


#include "config.h"
#include "system.h"
#include "coretypes.h"

#include "dfrontend/arraytypes.h"
#include "dfrontend/declaration.h"
#include "dfrontend/enum.h"
#include "dfrontend/module.h"

#include "tree.h"
#include "stringpool.h"

#include "d-tree.h"
#include "d-objfile.h"

// Implements the visitor interface to build debug trees for all module and
// import declarations, where ISYM holds the cached backend representation
// to be returned.

class ImportVisitor : public Visitor
{
public:
  ImportVisitor() {}

  // This should be overridden by each symbol class.
  void visit(Dsymbol *)
  {
    gcc_unreachable();
  }

  // Build the module namespace, this is considered toplevel, regardless
  // of whether there are any parent packages in the module system.
  void visit(Module *m)
  {
    m->isym = new Symbol();
    m->isym->prettyIdent = m->toPrettyChars();

    tree decl = build_decl(UNKNOWN_LOCATION, NAMESPACE_DECL,
			   get_identifier(m->isym->prettyIdent),
			   void_type_node);
    m->isym->Stree = decl;
    d_keep(decl);

    Loc loc = (m->md != NULL) ? m->md->loc
      : Loc(m->srcfile->toChars(), 1, 0);
    set_decl_location(decl, loc);

    if (!output_module_p(m))
      DECL_EXTERNAL (decl) = 1;

    TREE_PUBLIC (decl) = 1;
    DECL_CONTEXT (decl) = NULL_TREE;
  }

  // Alias symbols aren't imported, but their targets are.
  void visit(AliasDeclaration *d)
  {
    Dsymbol *dsym = d->toAlias();

    // This symbol is really an alias for another, visit the other.
    if (dsym != d)
      {
	dsym->accept(this);
	d->isym = dsym->isym;
	return;
      }

    // Type imports should really be part of their own visit method.
    if (d->type != NULL)
      {
	tree type = build_ctype(d->type);

	if (!TYPE_STUB_DECL (type))
	  return;

	tree decl = make_node(IMPORTED_DECL);
	TREE_TYPE (decl) = void_type_node;
	IMPORTED_DECL_ASSOCIATED_DECL (decl) = TYPE_STUB_DECL (type);
	d_keep(decl);

	d->isym = new Symbol();
	d->isym->Stree = decl;
	return;
      }
  }

  // Import a member of an enum declaration.
  void visit(EnumMember *m)
  {
    if (m->vd != NULL)
      {
	m->vd->accept(this);
	m->isym = m->vd->isym;
      }
  }

  // Skip over importing templates and tuples.
  void visit(TemplateDeclaration *)
  {
  }

  void visit(TupleDeclaration *)
  {
  }

  // Import any other kind of declaration.  If the class does not implement
  // symbol generation routines, the compiler will throw an error.
  void visit(Declaration *d)
  {
    tree decl = make_node(IMPORTED_DECL);
    TREE_TYPE (decl) = void_type_node;
    IMPORTED_DECL_ASSOCIATED_DECL (decl) = d->toSymbol()->Stree;
    d_keep(decl);

    d->isym = new Symbol();
    d->isym->Stree = decl;
  }
};


// Build a declaration for the symbol D that can be used for the debug_hook
// imported_module_or_decl.

tree
build_import_decl(Dsymbol *d)
{
  if (!d->isym)
    {
      ImportVisitor v;
      d->accept(&v);
    }

  // Not all visitors set 'isym'.
  return d->isym ? d->isym->Stree : NULL_TREE;
}

