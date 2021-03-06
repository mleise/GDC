// d-codegen.cc -- D frontend for GCC.
// Copyright (C) 2011-2015 Free Software Foundation, Inc.

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

#include "dfrontend/aggregate.h"
#include "dfrontend/attrib.h"
#include "dfrontend/declaration.h"
#include "dfrontend/init.h"
#include "dfrontend/module.h"
#include "dfrontend/statement.h"
#include "dfrontend/target.h"
#include "dfrontend/template.h"

#include "tree.h"
#include "tree-iterator.h"
#include "fold-const.h"
#include "diagnostic.h"
#include "langhooks.h"
#include "target.h"
#include "stringpool.h"
#include "stor-layout.h"
#include "attribs.h"
#include "function.h"

#include "d-tree.h"
#include "d-objfile.h"
#include "d-codegen.h"
#include "d-dmd-gcc.h"
#include "id.h"


Module *current_module_decl;


// Update data for defined and undefined labels when leaving a scope.

bool
pop_binding_label(Statement * const &, d_label_entry *ent, binding_level *bl)
{
  binding_level *obl = bl->level_chain;

  if (ent->level == bl)
    {
      if (bl->kind == level_try)
	ent->in_try_scope = true;
      else if (bl->kind == level_catch)
	ent->in_catch_scope = true;

      ent->level = obl;
    }
  else if (ent->fwdrefs)
    {
      for (d_label_use_entry *ref = ent->fwdrefs; ref; ref = ref->next)
	ref->level = obl;
    }

  return true;
}

// At the end of a function, all labels declared within the function
// go out of scope.  BLOCK is the top-level block for the function.

bool
pop_label(Statement * const &s, d_label_entry *ent, tree block)
{
  if (!ent->bc_label)
    {
      // Put the labels into the "variables" of the top-level block,
      // so debugger can see them.
      if (DECL_NAME (ent->label))
	{
	  gcc_assert(DECL_INITIAL (ent->label) != NULL_TREE);
	  DECL_CHAIN (ent->label) = BLOCK_VARS (block);
	  BLOCK_VARS (block) = ent->label;
	}
    }

  cfun->language->labels->remove(s);

  return true;
}

// The D front-end does not use the 'binding level' system for a symbol table,
// however it has been the goto structure for tracking code flow.
// Primarily it is only needed to get debugging information for local variables
// and otherwise support the backend.

void
push_binding_level(level_kind kind)
{
  // Add it to the front of currently active scopes stack.
  binding_level *new_level = ggc_cleared_alloc<binding_level>();
  new_level->level_chain = current_binding_level;
  new_level->kind = kind;

  current_binding_level = new_level;
}

tree
pop_binding_level()
{
  binding_level *level = current_binding_level;
  current_binding_level = level->level_chain;

  tree block = make_node(BLOCK);
  BLOCK_VARS (block) = level->names;
  BLOCK_SUBBLOCKS (block) = level->blocks;

  // In each subblock, record that this is its superior.
  for (tree t = level->blocks; t; t = BLOCK_CHAIN (t))
    BLOCK_SUPERCONTEXT (t) = block;

  if (level->kind == level_function)
    {
      // Dispose of the block that we just made inside some higher level.
      DECL_INITIAL (current_function_decl) = block;
      BLOCK_SUPERCONTEXT (block) = current_function_decl;

      // Pop all the labels declared in the function.
      if (cfun->language->labels)
	cfun->language->labels->traverse<tree, &pop_label>(block);
    }
  else
    {
      // Any uses of undefined labels, and any defined labels, now operate
      // under constraints of next binding contour.
      if (cfun && cfun->language->labels)
	{
	  cfun->language->labels->traverse<binding_level *, &pop_binding_label>
	    (level);
	}

      current_binding_level->blocks
	= block_chainon(current_binding_level->blocks, block);
    }

  TREE_USED (block) = 1;
  return block;
}


// Create an empty statement tree rooted at T.
void
push_stmt_list()
{
  tree t = alloc_stmt_list();
  vec_safe_push(cfun->language->stmt_list, t);
  d_keep(t);
}

// Finish the statement tree rooted at T.
tree
pop_stmt_list()
{
  tree t = cfun->language->stmt_list->pop();

  // If the statement list is completely empty, just return it.  This is
  // just as good small as build_empty_stmt, with the advantage that
  // statement lists are merged when they appended to one another.
  // So using the STATEMENT_LIST avoids pathological buildup of EMPTY_STMT_P
  // statements.
  if (TREE_SIDE_EFFECTS (t))
    {
      // If the statement list contained exactly one statement, then
      // extract it immediately.
      tree_stmt_iterator i = tsi_start (t);

      if (tsi_one_before_end_p (i))
	{
	  tree u = tsi_stmt (i);
	  tsi_delink (&i);
	  free_stmt_list (t);
	  t = u;
	}
    }

  return t;
}

// T is an expression statement.  Add it to the statement-tree.

void
add_stmt(tree t)
{
  // Ignore (void) 0; expression statements received from the frontend.
  // Likewise void_node is used when contracts become nops in release code.
  if (t == void_node || IS_EMPTY_STMT (t))
    return;

  if (EXPR_P (t) && !EXPR_HAS_LOCATION (t))
    SET_EXPR_LOCATION (t, input_location);

  tree stmt_list = cfun->language->stmt_list->last();
  append_to_statement_list_force(t, &stmt_list);
}

//
void
start_function(FuncDeclaration *decl)
{
  cfun->language = ggc_cleared_alloc<language_function>();
  cfun->language->function = decl;

  // Default chain value is 'null' unless parent found.
  cfun->language->static_chain = null_pointer_node;

  // Find module for this function
  for (Dsymbol *p = decl->parent; p != NULL; p = p->parent)
    {
      cfun->language->module = p->isModule();
      if (cfun->language->module)
	break;
    }
  gcc_assert(cfun->language->module != NULL);

  // Check if we have a static this or unitest function.
  ModuleInfo *mi = current_module_info;

  if (decl->isSharedStaticCtorDeclaration())
    mi->sharedctors.safe_push(decl);
  else if (decl->isStaticCtorDeclaration())
    mi->ctors.safe_push(decl);
  else if (decl->isSharedStaticDtorDeclaration())
    {
      VarDeclaration *vgate = ((SharedStaticDtorDeclaration *) decl)->vgate;
      if (vgate != NULL)
	mi->sharedctorgates.safe_push(vgate);
      mi->shareddtors.safe_push(decl);
    }
  else if (decl->isStaticDtorDeclaration())
    {
      VarDeclaration *vgate = ((StaticDtorDeclaration *) decl)->vgate;
      if (vgate != NULL)
	mi->ctorgates.safe_push(vgate);
      mi->dtors.safe_push(decl);
    }
  else if (decl->isUnitTestDeclaration())
    mi->unitTests.safe_push(decl);
}

void
end_function()
{
  gcc_assert(vec_safe_is_empty(cfun->language->stmt_list));

  ggc_free(cfun->language);
  cfun->language = NULL;
}


// Return the DECL_CONTEXT for symbol DSYM.

tree
d_decl_context (Dsymbol *dsym)
{
  Dsymbol *parent = dsym;
  Declaration *decl = dsym->isDeclaration();

  while ((parent = parent->toParent2()))
    {
      // We've reached the top-level module namespace.
      // Set DECL_CONTEXT as the NAMESPACE_DECL of the enclosing module,
      // but only for extern(D) symbols.
      if (parent->isModule())
	{
	  if (decl != NULL && decl->linkage != LINKd)
	    return NULL_TREE;

	  return build_import_decl(parent);
	}

      // Declarations marked as 'static' or '__gshared' are never
      // part of any context except at module level.
      if (decl != NULL && decl->isDataseg())
	continue;

      // Nested functions.
      if (parent->isFuncDeclaration())
	return parent->toSymbol()->Stree;

      // Methods of classes or structs.
      AggregateDeclaration *ad = parent->isAggregateDeclaration();
      if (ad != NULL)
	{
	  tree context = build_ctype(ad->type);
	  // Want the underlying RECORD_TYPE.
	  if (ad->isClassDeclaration())
	    context = TREE_TYPE (context);

	  return context;
	}
    }

  return NULL_TREE;
}

// Add local variable VD into the current function body.

void
build_local_var (VarDeclaration *vd)
{
  gcc_assert (!vd->isDataseg() && !vd->isMember());
  gcc_assert (current_function_decl != NULL_TREE);

  FuncDeclaration *fd = cfun->language->function;
  Symbol *sym = vd->toSymbol();
  tree var = sym->Stree;

  gcc_assert (!TREE_STATIC (var));

  set_input_location (vd->loc);
  d_pushdecl (var);
  DECL_CONTEXT (var) = current_function_decl;

  // Compiler generated symbols
  if (vd == fd->vresult || vd == fd->v_argptr)
    DECL_ARTIFICIAL (var) = 1;

  if (sym->SframeField)
    {
      // Fixes debugging local variables.
      SET_DECL_VALUE_EXPR (var, get_decl_tree (vd));
      DECL_HAS_VALUE_EXPR_P (var) = 1;
    }
}

// Return an unnamed local temporary of type TYPE.

tree
build_local_temp (tree type)
{
  tree decl = build_decl (BUILTINS_LOCATION, VAR_DECL, NULL_TREE, type);

  DECL_CONTEXT (decl) = current_function_decl;
  DECL_ARTIFICIAL (decl) = 1;
  DECL_IGNORED_P (decl) = 1;
  d_pushdecl (decl);

  return decl;
}

// Return an undeclared local temporary of type TYPE
// for use with BIND_EXPR.

tree
create_temporary_var (tree type)
{
  tree decl = build_decl (BUILTINS_LOCATION, VAR_DECL, NULL_TREE, type);
  DECL_CONTEXT (decl) = current_function_decl;
  DECL_ARTIFICIAL (decl) = 1;
  DECL_IGNORED_P (decl) = 1;
  layout_decl (decl, 0);
  return decl;
}

// Return an undeclared local temporary OUT_VAR initialised
// with result of expression EXP.

tree
maybe_temporary_var (tree exp, tree *out_var)
{
  tree t = exp;

  // Get the base component.
  while (TREE_CODE (t) == COMPONENT_REF)
    t = TREE_OPERAND (t, 0);

  if (!DECL_P (t) && !REFERENCE_CLASS_P (t))
    {
      *out_var = create_temporary_var (TREE_TYPE (exp));
      DECL_INITIAL (*out_var) = exp;
      return *out_var;
    }
  else
    {
      *out_var = NULL_TREE;
      return exp;
    }
}

// Emit an INIT_EXPR for decl DECL.

void
expand_decl (tree decl)
{
  // Nothing, d_pushdecl will add decl to a BIND_EXPR
  if (DECL_INITIAL (decl))
    {
      tree exp = build_vinit (decl, DECL_INITIAL (decl));
      add_stmt(exp);
      DECL_INITIAL (decl) = NULL_TREE;
    }
}

// Return the correct decl to be used for variable DECL accessed from the
// current function.  Could be a VAR_DECL, or a FIELD_DECL from a closure.

tree
get_decl_tree (Declaration *decl)
{
  VarDeclaration *vd = decl->isVarDeclaration();

  if (vd)
    {
      FuncDeclaration *func = cfun->language->function;
      Symbol *vsym = vd->toSymbol();
      if (vsym->SnamedResult != NULL_TREE)
	{
	  // Get the named return value.
	  gcc_assert (TREE_CODE (vsym->SnamedResult) == RESULT_DECL);
	  return vsym->SnamedResult;
	}
      else if (vsym->SframeField != NULL_TREE)
	{
	  // Get the closure holding the var decl.
	  FuncDeclaration *parent = vd->toParent2()->isFuncDeclaration();
	  tree frame_ref = get_framedecl (func, parent);

	  return component_ref (build_deref (frame_ref), vsym->SframeField);
	}
      else if (vd->parent != func && vd->isThisDeclaration() && func->isThis())
	{
	  // Get the non-local 'this' value by going through parent link
	  // of nested classes, this routine pretty much undoes what
	  // getRightThis in the frontend removes from codegen.
	  AggregateDeclaration *ad = func->isThis();
	  tree this_tree = func->vthis->toSymbol()->Stree;

	  while (true)
	    {
	      Dsymbol *outer = ad->toParent2();
	      // Get the this->this parent link.
	      tree vthis_field = ad->vthis->toSymbol()->Stree;
	      this_tree = component_ref (build_deref (this_tree), vthis_field);

	      ad = outer->isAggregateDeclaration();
	      if (ad != NULL)
		continue;

	      FuncDeclaration *fd = outer->isFuncDeclaration();
	      if (fd && fd->isThis())
		{
		  ad = fd->isThis();

		  // If outer function creates a closure, then the 'this' value
		  // would be the closure pointer, and the real 'this' the first
		  // field of that closure.
		  FuncFrameInfo *ff = get_frameinfo (fd);
		  if (ff->creates_frame)
		    {
		      this_tree = build_nop (build_pointer_type (ff->frame_rec), this_tree);
		      this_tree = indirect_ref (build_ctype(ad->type), this_tree);
		    }

		  // Continue looking for the right 'this'
		  if (fd != vd->parent)
		    continue;
		}

	      gcc_assert (outer == vd->parent);
	      return this_tree;
	    }
	}
    }

  // Static var or auto var that the back end will handle for us
  return decl->toSymbol()->Stree;
}

// Return expression EXP, whose type has been converted to TYPE.

tree
d_convert(tree type, tree exp)
{
  // Check this first before retrieving frontend type.
  if (error_operand_p(type) || error_operand_p(exp))
    return error_mark_node;

  Type *totype = TYPE_LANG_FRONTEND (type);
  Type *etype = TYPE_LANG_FRONTEND (TREE_TYPE (exp));

  if (totype && etype)
    return convert_expr(exp, etype, totype);

  return convert(type, exp);
}

// Return expression EXP, whose type has been convert from ETYPE to TOTYPE.

tree
convert_expr(tree exp, Type *etype, Type *totype)
{
  tree result = NULL_TREE;

  gcc_assert(etype && totype);
  Type *ebtype = etype->toBasetype();
  Type *tbtype = totype->toBasetype();

  if (d_types_same(etype, totype))
    return exp;

  if (error_operand_p(exp))
    return exp;

  switch (ebtype->ty)
    {
    case Tdelegate:
      if (tbtype->ty == Tdelegate)
	{
	  exp = maybe_make_temp(exp);
	  return build_delegate_cst(delegate_method(exp), delegate_object(exp), totype);
	}
      else if (tbtype->ty == Tpointer)
	{
	  // The front-end converts <delegate>.ptr to cast (void *)<delegate>.
	  // Maybe should only allow void* ?
	  exp = delegate_object(exp);
	}
      else
	{
	  error("can't convert a delegate expression to %s", totype->toChars());
	  return error_mark_node;
	}
      break;

    case Tstruct:
      if (tbtype->ty == Tstruct)
      {
	if (totype->size() == etype->size())
	  {
	    // Allowed to cast to structs with same type size.
	    result = build_vconvert(build_ctype(totype), exp);
	  }
	else
	  {
	    error("can't convert struct %s to %s", etype->toChars(), totype->toChars());
	    return error_mark_node;
	  }
      }
      // else, default conversion, which should produce an error
      break;

    case Tclass:
      if (tbtype->ty == Tclass)
      {
	ClassDeclaration *cdfrom = ebtype->isClassHandle();
	ClassDeclaration *cdto = tbtype->isClassHandle();
	int offset;

	if (cdfrom->cpp)
	  {
	    // Downcasting in C++ is a no-op.
	    if (cdto->cpp)
	      break;

	    // Casting from a C++ interface to a class/non-C++ interface
	    // always results in null as there is no runtime information,
	    // and no way one can derive from the other.
	    warning(OPT_Wcast_result, "cast to %s will produce null result", totype->toChars());
	    result = d_convert(build_ctype(totype), null_pointer_node);

	    // Make sure the expression is still evaluated if necessary
	    if (TREE_SIDE_EFFECTS(exp))
	      result = compound_expr(exp, result);

	    break;
	  }

	if (cdto->isBaseOf(cdfrom, &offset) && offset != OFFSET_RUNTIME)
	  {
	    // Casting up the inheritance tree: Don't do anything special.
	    // Cast to an implemented interface: Handle at compile time.
	    if (offset)
	      {
		tree type = build_ctype(totype);
		exp = maybe_make_temp(exp);

		tree cond = build_boolop(NE_EXPR, exp, null_pointer_node);
		tree object = build_offset(exp, size_int(offset));

		return build_condition(build_ctype(totype), cond,
				       build_nop(type, object),
				       build_nop(type, null_pointer_node));
	      }

	    // d_convert will make a no-op cast
	    break;
	  }

	// The offset can only be determined at runtime, do dynamic cast
	tree args[2];
	args[0] = exp;
	args[1] = build_address(cdto->toSymbol()->Stree);

	return build_libcall(cdfrom->isInterfaceDeclaration()
			     ? LIBCALL_INTERFACE_CAST : LIBCALL_DYNAMIC_CAST, 2, args);
      }
      // else default conversion
      break;

    case Tsarray:
      if (tbtype->ty == Tpointer)
	{
	  result = build_nop(build_ctype(totype), build_address(exp));
	}
      else if (tbtype->ty == Tarray)
	{
	  dinteger_t dim = ((TypeSArray *) ebtype)->dim->toInteger();
	  dinteger_t esize = ebtype->nextOf()->size();
	  dinteger_t tsize = tbtype->nextOf()->size();

	  tree ptrtype = build_ctype(tbtype->nextOf()->pointerTo());

	  if ((dim * esize) % tsize != 0)
	    {
	      error("cannot cast %s to %s since sizes don't line up",
		    etype->toChars(), totype->toChars());
	      return error_mark_node;
	    }
	  dim = (dim * esize) / tsize;

	  // Assumes casting to dynamic array of same type or void
	  return d_array_value(build_ctype(totype), size_int(dim),
			       build_nop(ptrtype, build_address(exp)));
	}
      else if (tbtype->ty == Tsarray)
	{
	  // D apparently allows casting a static array to any static array type
	  return build_vconvert(build_ctype(totype), exp);
	}
      else if (tbtype->ty == Tstruct)
	{
	  // And allows casting a static array to any struct type too.
	  // %% type sizes should have already been checked by the frontend.
	  gcc_assert(totype->size() == etype->size());
	  result = build_vconvert(build_ctype(totype), exp);
	}
      else
	{
	  error("cannot cast expression of type %s to type %s",
		etype->toChars(), totype->toChars());
	  return error_mark_node;
	}
      break;

    case Tarray:
      if (tbtype->ty == Tpointer)
	{
	  return d_convert(build_ctype(totype), d_array_ptr(exp));
	}
      else if (tbtype->ty == Tarray)
	{
	  // assume tvoid->size() == 1
	  Type *src_elem_type = ebtype->nextOf()->toBasetype();
	  Type *dst_elem_type = tbtype->nextOf()->toBasetype();
	  d_uns64 sz_src = src_elem_type->size();
	  d_uns64 sz_dst = dst_elem_type->size();

	  if (sz_src == sz_dst)
	    {
	      // Convert from void[] or elements are the same size -- don't change length
	      return build_vconvert(build_ctype(totype), exp);
	    }
	  else
	    {
	      unsigned mult = 1;
	      tree args[3];

	      args[0] = size_int(sz_dst);
	      args[1] = size_int(sz_src * mult);
	      args[2] = exp;

	      return build_libcall(LIBCALL_ARRAYCAST, 3, args, build_ctype(totype));
	    }
	}
      else if (tbtype->ty == Tsarray)
	{
	  // %% Strings are treated as dynamic arrays D2.
	  if (ebtype->isString() && tbtype->isString())
	    return indirect_ref(build_ctype(totype), d_array_ptr(exp));
	}
      else
	{
	  error("cannot cast expression of type %s to %s",
		etype->toChars(), totype->toChars());
	  return error_mark_node;
	}
      break;

    case Taarray:
      if (tbtype->ty == Taarray)
	return build_vconvert(build_ctype(totype), exp);
      // Can convert associative arrays to void pointers.
      else if (tbtype->ty == Tpointer && tbtype->nextOf()->ty == Tvoid)
	return build_vconvert(build_ctype(totype), exp);
      // else, default conversion, which should product an error
      break;

    case Tpointer:
      // Can convert void pointers to associative arrays too...
      if (tbtype->ty == Taarray && ebtype->nextOf()->ty == Tvoid)
	return build_vconvert(build_ctype(totype), exp);
      break;

    case Tnull:
      if (tbtype->ty == Tarray)
	{
	  tree ptrtype = build_ctype(tbtype->nextOf()->pointerTo());
	  return d_array_value(build_ctype(totype), size_int(0),
			       build_nop(ptrtype, exp));
	}
      else if (tbtype->ty == Taarray)
	  return build_vconvert(build_ctype(totype), exp);
      else if (tbtype->ty == Tdelegate)
	  return build_delegate_cst(exp, null_pointer_node, totype);
      break;

    case Tvector:
      if (tbtype->ty == Tsarray)
	{
	  if (tbtype->size() == ebtype->size())
	    return build_vconvert(build_ctype(totype), exp);
	}
      break;

    default:
      // All casts between imaginary and non-imaginary result in 0.0,
      // except for casts between complex and imaginary types.
      if (!ebtype->iscomplex() && !tbtype->iscomplex()
	  && (ebtype->isimaginary() != tbtype->isimaginary()))
	{
	  warning(OPT_Wcast_result, "cast from %s to %s will produce zero result",
		  ebtype->toChars(), tbtype->toChars());

	  return compound_expr(exp, build_zero_cst(build_ctype(tbtype)));
	}

      exp = fold_convert(build_ctype(etype), exp);
      gcc_assert(TREE_CODE (exp) != STRING_CST);
      break;
    }

  return result ? result :
    convert(build_ctype(totype), exp);
}


// Apply semantics of assignment to a values of type TOTYPE to EXPR
// (e.g., pointer = array -> pointer = &array[0])

// Return a TREE representation of EXPR implictly converted to TOTYPE
// for use in assignment expressions MODIFY_EXPR, INIT_EXPR...

tree
convert_for_assignment (tree expr, Type *etype, Type *totype)
{
  Type *ebtype = etype->toBasetype();
  Type *tbtype = totype->toBasetype();

  // Assuming this only has to handle converting a non Tsarray type to
  // arbitrarily dimensioned Tsarrays.
  if (tbtype->ty == Tsarray)
    {
      Type *telem = tbtype->nextOf()->baseElemOf();

      if (d_types_same (telem, ebtype))
	{
	  TypeSArray *sa_type = (TypeSArray *) tbtype;
	  uinteger_t count = sa_type->dim->toUInteger();

	  tree ctor = build_constructor (build_ctype(totype), NULL);
	  if (count)
	    {
	      vec<constructor_elt, va_gc> *ce = NULL;
	      tree index = build2 (RANGE_EXPR, build_ctype(Type::tsize_t),
				   size_zero_node, size_int(count - 1));
	      tree value = convert_for_assignment (expr, etype, sa_type->next);

	      // Can't use VAR_DECLs in CONSTRUCTORS.
	      if (VAR_P (value))
		{
		  value = DECL_INITIAL (value);
		  gcc_assert (value);
		}

	      CONSTRUCTOR_APPEND_ELT (ce, index, value);
	      CONSTRUCTOR_ELTS (ctor) = ce;
	    }
	  TREE_READONLY (ctor) = 1;
	  TREE_CONSTANT (ctor) = 1;
	  return ctor;
	}
    }

  // D Front end uses IntegerExp (0) to mean zero-init a structure.
  if (tbtype->ty == Tstruct && ebtype->isintegral())
    {
      if (!integer_zerop (expr))
	gcc_unreachable();

      return expr;
    }

  return convert_expr (expr, etype, totype);
}

// Return TRUE if TYPE is a static array va_list.  This is for compatibility
// with the C ABI, where va_list static arrays are passed by reference.
// However for ever other case in D, static arrays are passed by value.

static bool
type_va_array(Type *type)
{
  if (Type::tvalist->ty == Tsarray)
    {
      Type *tb = type->toBasetype();
      if (d_types_same(tb, Type::tvalist))
	return true;
    }

  return false;
}


// Return a TREE representation of EXPR converted to represent parameter type ARG.

tree
convert_for_argument(tree exp_tree, Expression *expr, Parameter *arg)
{
  if (type_va_array(arg->type))
    {
      // Do nothing if the va_list has already been decayed to a pointer.
      if (!POINTER_TYPE_P (TREE_TYPE (exp_tree)))
	return build_address(exp_tree);
    }
  else if (argument_reference_p(arg))
    {
      // Front-end sometimes automatically takes the address.
      if (expr->op != TOKaddress && expr->op != TOKsymoff && expr->op != TOKadd)
	exp_tree = build_address(exp_tree);

      return convert(type_passed_as(arg), exp_tree);
    }

  // Lazy arguments: expr should already be a delegate
  return exp_tree;
}

// Perform default promotions for data used in expressions.
// Arrays and functions are converted to pointers;
// enumeral types or short or char, to int.
// In addition, manifest constants symbols are replaced by their values.

// Return truth-value conversion of expression EXPR from value type TYPE.

tree
convert_for_condition(tree expr, Type *type)
{
  tree result = NULL_TREE;

  switch (type->toBasetype()->ty)
    {
    case Taarray:
      // Shouldn't this be...
      //  result = _aaLen (&expr);
      result = component_ref(expr, TYPE_FIELDS (TREE_TYPE (expr)));
      break;

    case Tarray:
    {
      // Checks (length || ptr) (i.e ary !is null)
      expr = maybe_make_temp(expr);
      tree len = d_array_length(expr);
      tree ptr = d_array_ptr(expr);
      if (TYPE_MODE (TREE_TYPE (len)) == TYPE_MODE (TREE_TYPE (ptr)))
	{
	  result = build2(BIT_IOR_EXPR, TREE_TYPE (len), len,
			  d_convert(TREE_TYPE (len), ptr));
	}
      else
	{
	  len = d_truthvalue_conversion(len);
	  ptr = d_truthvalue_conversion(ptr);
	  // probably not worth using TRUTH_OROR ...
	  result = build2(TRUTH_OR_EXPR, TREE_TYPE (len), len, ptr);
	}
      break;
    }

    case Tdelegate:
    {
      // Checks (function || object), but what good is it
      // if there is a null function pointer?
      tree obj, func;
      if (METHOD_CALL_EXPR (expr))
	extract_from_method_call(expr, obj, func);
      else
	{
	  expr = maybe_make_temp(expr);
	  obj = delegate_object(expr);
	  func = delegate_method(expr);
	}

      obj = d_truthvalue_conversion(obj);
      func = d_truthvalue_conversion(func);
      // probably not worth using TRUTH_ORIF ...
      result = build2(BIT_IOR_EXPR, TREE_TYPE (obj), obj, func);
      break;
    }

    default:
      result = expr;
      break;
    }

  return d_truthvalue_conversion (result);
}


// Convert EXP to a dynamic array.
// EXP must be a static array or dynamic array.

tree
d_array_convert(Expression *exp)
{
  Type *tb = exp->type->toBasetype();

  if (tb->ty == Tarray)
    return build_expr(exp);
  else if (tb->ty == Tsarray)
    {
      Type *totype = tb->nextOf()->arrayOf();
      return convert_expr(build_expr(exp), exp->type, totype);
    }

  // Invalid type passed.
  gcc_unreachable();
}

// Convert EXP to a dynamic array, where ETYPE is the element type.
// Similar to above, except that EXP is allowed to be an element of an array.
// Temporary variables that need some kind of BIND_EXPR are pushed to VARS.

tree
d_array_convert(Type *etype, Expression *exp, vec<tree, va_gc> **vars)
{
  Type *tb = exp->type->toBasetype();

  if ((tb->ty != Tarray && tb->ty != Tsarray)
      || d_types_same(tb, etype->toBasetype()))
    {
      // Convert single element to an array.
      tree var = NULL_TREE;
      tree expr = maybe_temporary_var(build_expr(exp), &var);

      if (var != NULL_TREE)
	vec_safe_push(*vars, var);

      return d_array_value(build_ctype(exp->type->arrayOf()),
			   size_int(1), build_address(expr));
    }
  else
    return d_array_convert(exp);
}

// Return TRUE if declaration DECL is a reference type.

bool
declaration_reference_p(Declaration *decl)
{
  Type *tb = decl->type->toBasetype();

  // Declaration is a reference type.
  if (tb->ty == Treference || decl->storage_class & (STCout | STCref))
    return true;

  return false;
}

// Returns the real type for declaration DECL.
// Reference decls are converted to reference-to-types.
// Lazy decls are converted into delegates.

tree
declaration_type(Declaration *decl)
{
  // Lazy declarations are converted to delegates.
  if (decl->storage_class & STClazy)
    {
      TypeFunction *tf = new TypeFunction(NULL, decl->type, false, LINKd);
      TypeDelegate *t = new TypeDelegate(tf);
      return build_ctype(t->merge());
    }

  // Static array va_list have array->pointer conversions applied.
  if (decl->isParameter() && type_va_array(decl->type))
    {
      Type *valist = decl->type->nextOf()->pointerTo();
      valist = valist->castMod(decl->type->mod);
      return build_ctype(valist);
    }

  tree type = build_ctype(decl->type);

  // Parameter is passed by reference.
  if (declaration_reference_p(decl))
    return build_reference_type(type);

  // The 'this' parameter is always const.
  if (decl->isThisDeclaration())
    return insert_type_modifiers(type, MODconst);

  return type;
}

// These should match the Declaration versions above
// Return TRUE if parameter ARG is a reference type.

bool
argument_reference_p(Parameter *arg)
{

  Type *tb = arg->type->toBasetype();

  // Parameter is a reference type.
  if (tb->ty == Treference || arg->storageClass & (STCout | STCref))
    return true;

  return false;
}

// Returns the real type for parameter ARG.
// Reference parameters are converted to reference-to-types.
// Lazy parameters are converted into delegates.

tree
type_passed_as(Parameter *arg)
{
  // Lazy parameters are converted to delegates.
  if (arg->storageClass & STClazy)
    {
      TypeFunction *tf = new TypeFunction(NULL, arg->type, false, LINKd);
      TypeDelegate *t = new TypeDelegate(tf);
      return build_ctype(t->merge());
    }

  // Static array va_list have array->pointer conversions applied.
  if (type_va_array(arg->type))
    {
      Type *valist = arg->type->nextOf()->pointerTo();
      valist = valist->castMod(arg->type->mod);
      return build_ctype(valist);
    }

  tree type = build_ctype(arg->type);

  // Parameter is passed by reference.
  if (argument_reference_p(arg))
    return build_reference_type(type);

  return type;
}

// Returns an array of type D_TYPE which has SIZE number of elements.

tree
d_array_type(Type *d_type, uinteger_t size)
{
  tree index_type_node;
  tree type_node = build_ctype(d_type);

  if (size > 0)
    {
      index_type_node = size_int(size - 1);
      index_type_node = build_index_type(index_type_node);
    }
  else
    {
      tree type = lang_hooks.types.type_for_size(TYPE_PRECISION (sizetype),
						 TYPE_UNSIGNED (sizetype));

      index_type_node = build_range_type(type, size_zero_node, NULL_TREE);
    }

  tree array_type = build_array_type(type_node, index_type_node);

  if (size == 0)
    {
      TYPE_SIZE (array_type) = bitsize_zero_node;
      TYPE_SIZE_UNIT (array_type) = size_zero_node;
    }

  return array_type;
}

// Appends the type attribute ATTRNAME with value VALUE onto type TYPE.

tree
insert_type_attribute (tree type, const char *attrname, tree value)
{
  tree attrib;
  tree ident = get_identifier (attrname);

  if (value)
    value = tree_cons (NULL_TREE, value, NULL_TREE);

  // types built by functions in tree.c need to be treated as immutabl
  if (!TYPE_ATTRIBUTES (type))
    type = build_variant_type_copy (type);

  attrib = tree_cons (ident, value, NULL_TREE);
  TYPE_ATTRIBUTES (type) = merge_attributes (TYPE_ATTRIBUTES (type), attrib);

  return type;
}

// Appends the decl attribute ATTRNAME with value VALUE onto decl DECL.

void
insert_decl_attribute (tree decl, const char *attrname, tree value)
{
  tree attrib;
  tree ident = get_identifier (attrname);

  if (value)
    value = tree_cons (NULL_TREE, value, NULL_TREE);

  attrib = tree_cons (ident, value, NULL_TREE);
  DECL_ATTRIBUTES (decl) = merge_attributes (DECL_ATTRIBUTES (decl), attrib);
}

bool
d_attribute_p (const char* name)
{
  static StringTable* table;

  if(table == NULL)
    {
      // Build the table of attributes exposed to the language.
      // Common and format attributes are kept internal.
      size_t n = 0;
      table = new StringTable();

      for (const attribute_spec *p = lang_hooks.attribute_table; p->name; p++)
	n++;

      for (const attribute_spec *p = targetm.attribute_table; p->name; p++)
	n++;

      if(n != 0)
	{
	  table->_init(n);

	  for (const attribute_spec *p = lang_hooks.attribute_table; p->name; p++)
	    table->insert(p->name, strlen(p->name));

	  for (const attribute_spec *p = targetm.attribute_table; p->name; p++)
	    table->insert(p->name, strlen(p->name));
	}
    }

  return table->lookup(name, strlen(name)) != NULL;
}

// Return chain of all GCC attributes found in list IN_ATTRS.

tree
build_attributes (Expressions *in_attrs)
{
  if (!in_attrs)
    return NULL_TREE;

  expandTuples(in_attrs);

  tree out_attrs = NULL_TREE;

  for (size_t i = 0; i < in_attrs->dim; i++)
    {
      Expression *attr = (*in_attrs)[i];
      Dsymbol *sym = attr->type->toDsymbol (0);

      if (!sym)
	continue;

      Dsymbol *mod = (Dsymbol*) sym->getModule();
      if (!(strcmp(mod->toChars(), "attribute") == 0
          && mod->parent != NULL
          && strcmp(mod->parent->toChars(), "gcc") == 0
          && !mod->parent->parent))
        continue;

      if (attr->op == TOKcall)
	attr = attr->ctfeInterpret();

      gcc_assert(attr->op == TOKstructliteral);
      Expressions *elem = ((StructLiteralExp*) attr)->elements;

      if ((*elem)[0]->op == TOKnull)
	{
	  error ("expected string attribute, not null");
	  return error_mark_node;
	}

      gcc_assert((*elem)[0]->op == TOKstring);
      StringExp *nameExp = (StringExp*) (*elem)[0];
      gcc_assert(nameExp->sz == 1);
      const char* name = (const char*) nameExp->string;

      if (!d_attribute_p (name))
      {
        error ("unknown attribute %s", name);
        return error_mark_node;
      }

      tree args = NULL_TREE;

      for (size_t j = 1; j < elem->dim; j++)
        {
	  Expression *ae = (*elem)[j];
	  tree aet;
	  if (ae->op == TOKstring && ((StringExp *) ae)->sz == 1)
	    {
	      StringExp *s = (StringExp *) ae;
	      aet = build_string (s->len, (const char *) s->string);
	    }
	  else
	    aet = build_expr(ae);

	  args = chainon (args, build_tree_list (0, aet));
        }

      tree list = build_tree_list (get_identifier (name), args);
      out_attrs =  chainon (out_attrs, list);
    }

  return out_attrs;
}

// Return qualified type variant of TYPE determined by modifier value MOD.

tree
insert_type_modifiers (tree type, unsigned mod)
{
  int quals = 0;
  gcc_assert (type);

  switch (mod)
    {
    case 0:
      break;

    case MODconst:
    case MODwild:
    case MODwildconst:
    case MODimmutable:
      quals |= TYPE_QUAL_CONST;
      break;

    case MODshared:
      quals |= TYPE_QUAL_VOLATILE;
      break;

    case MODshared | MODconst:
    case MODshared | MODwild:
    case MODshared | MODwildconst:
      quals |= TYPE_QUAL_CONST;
      quals |= TYPE_QUAL_VOLATILE;
      break;

    default:
      gcc_unreachable();
    }

  return build_qualified_type (type, quals);
}

// Build INTEGER_CST of type TYPE with the value VALUE.

tree
build_integer_cst (dinteger_t value, tree type)
{
  // The type is error_mark_node, we can't do anything.
  if (error_operand_p (type))
    return type;

  return build_int_cst_type (type, value);
}

// Build REAL_CST of type TOTYPE with the value VALUE.

tree
build_float_cst (const real_t& value, Type *totype)
{
  real_t new_value;
  TypeBasic *tb = totype->isTypeBasic();

  gcc_assert (tb != NULL);

  tree type_node = build_ctype(tb);
  real_convert (&new_value.rv(), TYPE_MODE (type_node), &value.rv());

  // Value grew as a result of the conversion. %% precision bug ??
  // For now just revert back to original.
  if (new_value > value)
    new_value = value;

  return build_real (type_node, new_value.rv());
}

// Returns the .length component from the D dynamic array EXP.

tree
d_array_length (tree exp)
{
  // backend will ICE otherwise
  if (error_operand_p (exp))
    return exp;

  // Get the backend type for the array and pick out the array
  // length field (assumed to be the first field.)
  tree len_field = TYPE_FIELDS (TREE_TYPE (exp));
  return component_ref (exp, len_field);
}

// Returns the .ptr component from the D dynamic array EXP.

tree
d_array_ptr (tree exp)
{
  // backend will ICE otherwise
  if (error_operand_p (exp))
    return exp;

  // Get the backend type for the array and pick out the array
  // data pointer field (assumed to be the second field.)
  tree ptr_field = TREE_CHAIN (TYPE_FIELDS (TREE_TYPE (exp)));
  return component_ref (exp, ptr_field);
}

// Returns a constructor for D dynamic array type TYPE of .length LEN
// and .ptr pointing to DATA.

tree
d_array_value (tree type, tree len, tree data)
{
  // %% assert type is a darray
  tree len_field, ptr_field;
  vec<constructor_elt, va_gc> *ce = NULL;

  len_field = TYPE_FIELDS (type);
  ptr_field = TREE_CHAIN (len_field);

  len = convert (TREE_TYPE (len_field), len);
  data = convert (TREE_TYPE (ptr_field), data);

  CONSTRUCTOR_APPEND_ELT (ce, len_field, len);
  CONSTRUCTOR_APPEND_ELT (ce, ptr_field, data);

  tree ctor = build_constructor (type, ce);
  // TREE_STATIC and TREE_CONSTANT can be set by caller if needed
  TREE_STATIC (ctor) = 0;
  TREE_CONSTANT (ctor) = 0;

  return ctor;
}

// Builds a D string value from the C string STR.

tree
d_array_string (const char *str)
{
  unsigned len = strlen (str);
  // Assumes STR is 0-terminated.
  tree str_tree = build_string (len + 1, str);

  TREE_TYPE (str_tree) = d_array_type (Type::tchar, len);

  return d_array_value (build_ctype(Type::tchar->arrayOf()),
			size_int (len), build_address (str_tree));
}

// Returns value representing the array length of expression EXP.
// TYPE could be a dynamic or static array.

tree
get_array_length (tree exp, Type *type)
{
  Type *tb = type->toBasetype();

  switch (tb->ty)
    {
    case Tsarray:
      return size_int (((TypeSArray *) tb)->dim->toUInteger());

    case Tarray:
      return d_array_length (exp);

    default:
      error ("can't determine the length of a %s", type->toChars());
      return error_mark_node;
    }
}

// Create BINFO for a ClassDeclaration's inheritance tree.
// Interfaces are not included.

tree
build_class_binfo (tree super, ClassDeclaration *cd)
{
  tree binfo = make_tree_binfo (1);
  tree ctype = build_ctype(cd->type);

  // Want RECORD_TYPE, not POINTER_TYPE
  BINFO_TYPE (binfo) = TREE_TYPE (ctype);
  BINFO_INHERITANCE_CHAIN (binfo) = super;
  BINFO_OFFSET (binfo) = integer_zero_node;

  if (cd->baseClass)
    BINFO_BASE_APPEND (binfo, build_class_binfo (binfo, cd->baseClass));

  return binfo;
}

// Create BINFO for an InterfaceDeclaration's inheritance tree.
// In order to access all inherited methods in the debugger,
// the entire tree must be described.
// This function makes assumptions about interface layout.

tree
build_interface_binfo (tree super, ClassDeclaration *cd, unsigned& offset)
{
  tree binfo = make_tree_binfo (cd->baseclasses->dim);
  tree ctype = build_ctype(cd->type);

  // Want RECORD_TYPE, not POINTER_TYPE
  BINFO_TYPE (binfo) = TREE_TYPE (ctype);
  BINFO_INHERITANCE_CHAIN (binfo) = super;
  BINFO_OFFSET (binfo) = size_int (offset * Target::ptrsize);
  BINFO_VIRTUAL_P (binfo) = 1;

  for (size_t i = 0; i < cd->baseclasses->dim; i++, offset++)
    {
      BaseClass *bc = (*cd->baseclasses)[i];
      BINFO_BASE_APPEND (binfo, build_interface_binfo (binfo, bc->base, offset));
    }

  return binfo;
}

// Returns the .funcptr component from the D delegate EXP.

tree
delegate_method (tree exp)
{
  // Get the backend type for the array and pick out the array length
  // field (assumed to be the second field.)
  tree method_field = TREE_CHAIN (TYPE_FIELDS (TREE_TYPE (exp)));
  return component_ref (exp, method_field);
}

// Returns the .object component from the delegate EXP.

tree
delegate_object (tree exp)
{
  // Get the backend type for the array and pick out the array data
  // pointer field (assumed to be the first field.)
  tree obj_field = TYPE_FIELDS (TREE_TYPE (exp));
  return component_ref (exp, obj_field);
}

// Build a delegate literal of type TYPE whose pointer function is
// METHOD, and hidden object is OBJECT.

tree
build_delegate_cst(tree method, tree object, Type *type)
{
  tree ctor = make_node(CONSTRUCTOR);
  tree ctype;

  Type *tb = type->toBasetype();
  if (tb->ty == Tdelegate)
    ctype = build_ctype(type);
  else
    {
      // Convert a function literal into an anonymous delegate.
      ctype = build_two_field_type(TREE_TYPE (object), TREE_TYPE (method),
				   NULL, "object", "func");
    }

  vec<constructor_elt, va_gc> *ce = NULL;
  CONSTRUCTOR_APPEND_ELT (ce, TYPE_FIELDS (ctype), object);
  CONSTRUCTOR_APPEND_ELT (ce, TREE_CHAIN (TYPE_FIELDS (ctype)), method);

  CONSTRUCTOR_ELTS (ctor) = ce;
  TREE_TYPE (ctor) = ctype;

  return ctor;
}

// Builds a temporary tree to store the CALLEE and OBJECT
// of a method call expression of type TYPE.

tree
build_method_call (tree callee, tree object, Type *type)
{
  tree t = build_delegate_cst (callee, object, type);
  METHOD_CALL_EXPR (t) = 1;
  return t;
}

// Extract callee and object from T and return in to CALLEE and OBJECT.

void
extract_from_method_call (tree t, tree& callee, tree& object)
{
  gcc_assert (METHOD_CALL_EXPR (t));
  object = CONSTRUCTOR_ELT (t, 0)->value;
  callee = CONSTRUCTOR_ELT (t, 1)->value;
}

// Build a dereference into the virtual table for OBJECT to retrieve
// a function pointer of type FNTYPE at position INDEX.

tree
build_vindex_ref(tree object, tree fntype, size_t index)
{
  // Interface methods are also in the class's vtable, so we don't
  // need to convert from a class pointer to an interface pointer.
  object = maybe_make_temp(object);

  // The vtable is the first field.
  tree result = build_deref(object);
  result = component_ref(result, TYPE_FIELDS (TREE_TYPE (result)));

  gcc_assert(POINTER_TYPE_P (fntype));

  return build_memref(fntype, result, size_int(Target::ptrsize * index));
}

// Builds a record type from field types T1 and T2.  TYPE is the D frontend
// type we are building. N1 and N2 are the names of the two fields.

tree
build_two_field_type(tree t1, tree t2, Type *type, const char *n1, const char *n2)
{
  tree rectype = make_node(RECORD_TYPE);
  tree f0 = build_decl(BUILTINS_LOCATION, FIELD_DECL, get_identifier(n1), t1);
  tree f1 = build_decl(BUILTINS_LOCATION, FIELD_DECL, get_identifier(n2), t2);

  DECL_FIELD_CONTEXT(f0) = rectype;
  DECL_FIELD_CONTEXT(f1) = rectype;
  TYPE_FIELDS(rectype) = chainon(f0, f1);
  if (type != NULL)
    TYPE_NAME(rectype) = get_identifier(type->toChars());
  layout_type(rectype);

  return rectype;
}

// Create a SAVE_EXPR if EXP might have unwanted side effects if referenced
// more than once in an expression.

tree
make_temp (tree exp)
{
  if (TREE_CODE (exp) == CALL_EXPR
      || TREE_CODE (TREE_TYPE (exp)) != ARRAY_TYPE)
    return save_expr (exp);
  else
    return stabilize_reference (exp);
}

tree
maybe_make_temp (tree exp)
{
  if (d_has_side_effects (exp))
    return make_temp (exp);

  return exp;
}

// Return TRUE if EXP can not be evaluated multiple times (i.e., in a loop body)
// without unwanted side effects.

bool
d_has_side_effects (tree exp)
{
  tree t = STRIP_NOPS (exp);

  // SAVE_EXPR is safe to reference more than once, but not to
  // expand in a loop.
  if (TREE_CODE (t) == SAVE_EXPR)
    return false;

  if (DECL_P (t)
      || CONSTANT_CLASS_P (t))
    return false;

  if (INDIRECT_REF_P (t)
      || TREE_CODE (t) == ADDR_EXPR
      || TREE_CODE (t) == COMPONENT_REF)
    return d_has_side_effects (TREE_OPERAND (t, 0));

  return TREE_SIDE_EFFECTS (t);
}


// Returns the address of the expression EXP.

tree
build_address(tree exp)
{
  tree ptrtype;
  tree type = TREE_TYPE (exp);
  d_mark_addressable(exp);

  if (TREE_CODE (exp) == STRING_CST)
    {
      // Just convert string literals (char[]) to C-style strings (char *),
      // otherwise the latter method (char[]*) causes conversion problems
      // during gimplification.
      ptrtype = build_pointer_type(TREE_TYPE (type));
    }
  else if (TYPE_MAIN_VARIANT (type) == TYPE_MAIN_VARIANT (va_list_type_node)
	   && TREE_CODE (TYPE_MAIN_VARIANT (type)) == ARRAY_TYPE)
    {
      // Special case for va_list.  Backend will be expects a pointer to va_list,
      // but some targets use an array type.  So decay the array to pointer.
      ptrtype = build_pointer_type(TREE_TYPE (type));
    }
  else
    ptrtype = build_pointer_type(type);

  tree ret = build_fold_addr_expr_with_type_loc(input_location, exp, ptrtype);

  if (TREE_CODE (exp) == FUNCTION_DECL)
    TREE_NO_TRAMPOLINE (ret) = 1;

  return ret;
}

tree
d_mark_addressable (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case ADDR_EXPR:
    case COMPONENT_REF:
      /* If D had bit fields, we would need to handle that here */
    case ARRAY_REF:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 0));
      break;

      /* %% C++ prevents {& this} .... */
    case TRUTH_ANDIF_EXPR:
    case TRUTH_ORIF_EXPR:
    case COMPOUND_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 1));
      break;

    case COND_EXPR:
      d_mark_addressable (TREE_OPERAND (exp, 1));
      d_mark_addressable (TREE_OPERAND (exp, 2));
      break;

    case CONSTRUCTOR:
      TREE_ADDRESSABLE (exp) = 1;
      break;

    case INDIRECT_REF:
      /* %% this was in Java, not sure for D */
      /* We sometimes add a cast *(TYPE *)&FOO to handle type and mode
	 incompatibility problems.  Handle this case by marking FOO.  */
      if (TREE_CODE (TREE_OPERAND (exp, 0)) == NOP_EXPR
	  && TREE_CODE (TREE_OPERAND (TREE_OPERAND (exp, 0), 0)) == ADDR_EXPR)
	{
	  d_mark_addressable (TREE_OPERAND (TREE_OPERAND (exp, 0), 0));
	  break;
	}
      if (TREE_CODE (TREE_OPERAND (exp, 0)) == ADDR_EXPR)
	{
	  d_mark_addressable (TREE_OPERAND (exp, 0));
	  break;
	}
      break;

    case VAR_DECL:
    case CONST_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case FUNCTION_DECL:
      TREE_USED (exp) = 1;
      TREE_ADDRESSABLE (exp) = 1;

      /* drops through */
    default:
      break;
    }

  return exp;
}

/* Mark EXP as "used" in the program for the benefit of
   -Wunused warning purposes.  */

tree
d_mark_used (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case VAR_DECL:
    case PARM_DECL:
      TREE_USED (exp) = 1;
      break;

    case ARRAY_REF:
    case COMPONENT_REF:
    case MODIFY_EXPR:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
    case NOP_EXPR:
    case CONVERT_EXPR:
    case ADDR_EXPR:
      d_mark_used (TREE_OPERAND (exp, 0));
      break;

    case COMPOUND_EXPR:
      d_mark_used (TREE_OPERAND (exp, 0));
      d_mark_used (TREE_OPERAND (exp, 1));
      break;

    default:
      break;
    }
  return exp;
}

/* Mark EXP as read, not just set, for set but not used -Wunused
   warning purposes.  */

tree
d_mark_read (tree exp)
{
  switch (TREE_CODE (exp))
    {
    case VAR_DECL:
    case PARM_DECL:
      TREE_USED (exp) = 1;
      DECL_READ_P (exp) = 1;
      break;

    case ARRAY_REF:
    case COMPONENT_REF:
    case MODIFY_EXPR:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
    case NOP_EXPR:
    case CONVERT_EXPR:
    case ADDR_EXPR:
      d_mark_read (TREE_OPERAND (exp, 0));
      break;

    case COMPOUND_EXPR:
      d_mark_read (TREE_OPERAND (exp, 1));
      break;

    default:
      break;
    }
  return exp;
}

// Return TRUE if the struct SD is suitable for comparison using memcmp.
// This is because we don't guarantee that padding is zero-initialized for
// a stack variable, so we can't use memcmp to compare struct values.

bool
identity_compare_p(StructDeclaration *sd)
{
  if (sd->isUnionDeclaration())
    return true;

  unsigned offset = 0;

  for (size_t i = 0; i < sd->fields.dim; i++)
    {
      VarDeclaration *vd = sd->fields[i];

      // Check inner data structures.
      if (vd->type->ty == Tstruct)
	{
	  TypeStruct *ts = (TypeStruct *) vd->type;
	  if (!identity_compare_p(ts->sym))
	    return false;
	}

      if (offset <= vd->offset)
	{
	  // There's a hole in the struct.
	  if (offset != vd->offset)
	    return false;

	  offset += vd->type->size();
	}
    }

  // Any trailing padding may not be zero.
  if (offset < sd->structsize)
    return false;

  return true;
}

// Lower a field-by-field equality expression between T1 and T2 of type SD.
// CODE is the EQ_EXPR or NE_EXPR comparison.

static tree
lower_struct_comparison(tree_code code, StructDeclaration *sd, tree t1, tree t2)
{
  tree_code tcode = (code == EQ_EXPR) ? TRUTH_ANDIF_EXPR : TRUTH_ORIF_EXPR;
  tree tmemcmp = NULL_TREE;

  // We can skip the compare if the structs are empty
  if (sd->fields.dim == 0)
    return build_boolop(code, integer_zero_node, integer_zero_node);

  // Let backend take care of union comparisons.
  if (sd->isUnionDeclaration())
    {
      tmemcmp = d_build_call_nary(builtin_decl_explicit(BUILT_IN_MEMCMP), 3,
				  build_address(t1), build_address(t2),
				  size_int(sd->structsize));

      return build_boolop(code, tmemcmp, integer_zero_node);
    }

  for (size_t i = 0; i < sd->fields.dim; i++)
    {
      VarDeclaration *vd = sd->fields[i];
      tree sfield = vd->toSymbol()->Stree;

      tree t1ref = component_ref(t1, sfield);
      tree t2ref = component_ref(t2, sfield);
      tree tcmp;

      if (vd->type->ty == Tstruct)
	{
	  // Compare inner data structures.
	  StructDeclaration *decl = ((TypeStruct *) vd->type)->sym;
	  tcmp = lower_struct_comparison(code, decl, t1ref, t2ref);
	}
      else
	{
	  tree stype = build_ctype(vd->type);
	  machine_mode mode = int_mode_for_mode(TYPE_MODE (stype));

	  if (vd->type->isintegral())
	    {
	      // Integer comparison, no special handling required.
	      tcmp = build_boolop(code, t1ref, t2ref);
	    }
	  else if (mode != BLKmode)
	    {
	      // Compare field bits as their corresponding integer type.
	      //   *((T*) &t1) == *((T*) &t2)
	      tree tmode = lang_hooks.types.type_for_mode(mode, 1);

	      if (tmode == NULL_TREE)
		tmode = make_unsigned_type(GET_MODE_BITSIZE (mode));

	      t1ref = build_vconvert(tmode, t1ref);
	      t2ref = build_vconvert(tmode, t2ref);

	      tcmp = build_boolop(code, t1ref, t2ref);
	    }
	  else
	    {
	      // Simple memcmp between types.
	      tcmp = d_build_call_nary(builtin_decl_explicit(BUILT_IN_MEMCMP), 3,
				       build_address(t1ref), build_address(t2ref),
				       TYPE_SIZE_UNIT (stype));

	      tcmp = build_boolop(code, tcmp, integer_zero_node);
	    }
	}

      tmemcmp = (tmemcmp) ? build_boolop(tcode, tmemcmp, tcmp) : tcmp;
    }

  return tmemcmp;
}


// Build an equality expression between two RECORD_TYPES T1 and T2 of type SD.
// If possible, use memcmp, otherwise field-by-field comparison is done.
// CODE is the EQ_EXPR or NE_EXPR comparison.

tree
build_struct_comparison(tree_code code, StructDeclaration *sd, tree t1, tree t2)
{
  // We can skip the compare if the structs are empty
  if (sd->fields.dim == 0)
    return build_boolop(code, integer_zero_node, integer_zero_node);

  // Bitwise comparison of structs not returned in memory may not work
  // due to data holes loosing its zero padding upon return.
  // As a heuristic, small structs are not compared using memcmp either.
  if (TYPE_MODE (TREE_TYPE (t1)) != BLKmode || !identity_compare_p(sd))
    {
      // Make temporaries to prevent multiple evaluations.
      t1 = maybe_make_temp(t1);
      t2 = maybe_make_temp(t2);

      return lower_struct_comparison(code, sd, t1, t2);
    }
  else
    {
      // Do bit compare of structs.
      tree size = size_int(sd->structsize);
      tree tmemcmp = d_build_call_nary(builtin_decl_explicit(BUILT_IN_MEMCMP), 3,
				       build_address(t1), build_address(t2), size);

      return build_boolop(code, tmemcmp, integer_zero_node);
    }
}

// Build an equality expression between two ARRAY_TYPES of size LENGTH.
// The pointer references are T1 and T2, and the element type is SD.
// CODE is the EQ_EXPR or NE_EXPR comparison.

tree
build_array_struct_comparison(tree_code code, StructDeclaration *sd,
			      tree length, tree t1, tree t2)
{
  tree_code tcode = (code == EQ_EXPR) ? TRUTH_ANDIF_EXPR : TRUTH_ORIF_EXPR;

  // Build temporary for the result of the comparison.
  // Initialize as either 0 or 1 depending on operation.
  tree result = build_local_temp(bool_type_node);
  tree init = build_boolop(code, integer_zero_node, integer_zero_node);
  add_stmt(build_vinit(result, init));

  // Cast pointer-to-array to pointer-to-struct.
  tree ptrtype = build_ctype(sd->type->pointerTo());
  tree lentype = TREE_TYPE (length);

  push_binding_level(level_block);
  push_stmt_list();

  // Build temporary locals for length and pointers.
  tree t = build_local_temp(size_type_node);
  add_stmt(build_vinit(t, length));
  length = t;

  t = build_local_temp(ptrtype);
  add_stmt(build_vinit(t, d_convert(ptrtype, t1)));
  t1 = t;

  t = build_local_temp(ptrtype);
  add_stmt(build_vinit(t, d_convert(ptrtype, t2)));
  t2 = t;

  // Build loop for comparing each element.
  push_stmt_list();

  // Exit logic for the loop.
  //	if (length == 0 || result OP 0) break
  t = build_boolop(EQ_EXPR, length, d_convert(lentype, integer_zero_node));
  t = build_boolop(TRUTH_ORIF_EXPR, t, build_boolop(code, result, boolean_false_node));
  t = build1(EXIT_EXPR, void_type_node, t);
  add_stmt(t);

  // Do comparison, caching the value.
  //	result = result OP (*t1 == *t2)
  t = build_struct_comparison(code, sd, build_deref(t1), build_deref(t2));
  t = build_boolop(tcode, result, t);
  t = vmodify_expr(result, t);
  add_stmt(t);

  // Move both pointers to next element position.
  //	t1++, t2++;
  tree size = d_convert(ptrtype, TYPE_SIZE_UNIT (TREE_TYPE (ptrtype)));
  t = build2(POSTINCREMENT_EXPR, ptrtype, t1, size);
  add_stmt(t);
  t = build2(POSTINCREMENT_EXPR, ptrtype, t2, size);
  add_stmt(t);

  // Decrease loop counter.
  //	length -= 1
  t = build2(POSTDECREMENT_EXPR, lentype, length,
	     d_convert(lentype, integer_one_node));
  add_stmt(t);

  // Pop statements and finish loop.
  tree body = pop_stmt_list();
  add_stmt(build1(LOOP_EXPR, void_type_node, body));

  // Wrap it up into a bind expression.
  tree stmt_list = pop_stmt_list();
  tree block = pop_binding_level();

  body = build3(BIND_EXPR, void_type_node,
		BLOCK_VARS (block), stmt_list, block);

  return compound_expr(body, result);
}

// Build a constructor for a variable of aggregate type TYPE using the
// initializer INIT, an ordered flat list of fields and values provided
// by the frontend.
// The returned constructor should be a value that matches the layout of TYPE.

tree
build_struct_literal(tree type, tree init)
{
  // If the initializer was empty, use default zero initialization.
  if (vec_safe_is_empty(CONSTRUCTOR_ELTS (init)))
    return build_constructor(type, NULL);

  vec<constructor_elt, va_gc> *ve = NULL;

  // Walk through each field, matching our initializer list
  for (tree field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
    {
      gcc_assert(!vec_safe_is_empty(CONSTRUCTOR_ELTS (init)));
      constructor_elt *ce = &(*CONSTRUCTOR_ELTS (init))[0];
      tree value = NULL_TREE;

      // Found the next field to initialize, consume the value and
      // pop it from the init list.
      if (ce->index == field)
	{
	  value = ce->value;
	  CONSTRUCTOR_ELTS (init)->ordered_remove(0);
	}
      else if (DECL_NAME (field) == NULL_TREE)
	{
	  // Search all nesting aggregates, if nothing is found, then
	  // this will return an empty initializer to fill the hole.
	  if (RECORD_OR_UNION_TYPE_P (TREE_TYPE (field))
	      && ANON_AGGR_TYPE_P (TREE_TYPE (field)))
	    value = build_struct_literal(TREE_TYPE (field), init);
	  else
	    value = build_constructor(TREE_TYPE (field), NULL);
	}

      if (value != NULL_TREE)
	{
	  CONSTRUCTOR_APPEND_ELT (ve, field, value);
	  if (vec_safe_is_empty(CONSTRUCTOR_ELTS (init)))
	    break;
	}
    }

  // Ensure that we have consumed all values.
  gcc_assert(vec_safe_is_empty(CONSTRUCTOR_ELTS (init))
	     || ANON_AGGR_TYPE_P (type));

  return build_constructor(type, ve);
}

// Given the TYPE of an anonymous field inside T, return the
// FIELD_DECL for the field.  If not found return NULL_TREE.
// Because anonymous types can nest, we must also search all
// anonymous fields that are directly reachable.

static tree
lookup_anon_field(tree t, tree type)
{
  t = TYPE_MAIN_VARIANT (t);

  for (tree field = TYPE_FIELDS (t); field; field = DECL_CHAIN (field))
    {
      if (DECL_NAME (field) == NULL_TREE)
	{
	  // If we find it directly, return the field.
	  if (type == TYPE_MAIN_VARIANT (TREE_TYPE (field)))
	    return field;

	  // Otherwise, it could be nested, search harder.
	  if (RECORD_OR_UNION_TYPE_P (TREE_TYPE (field))
	      && ANON_AGGR_TYPE_P (TREE_TYPE (field)))
	    {
	      tree subfield = lookup_anon_field(TREE_TYPE (field), type);
	      if (subfield)
		return subfield;
	    }
	}
    }

  return NULL_TREE;
}

// Builds OBJECT.FIELD component reference.

tree
component_ref(tree object, tree field)
{
  gcc_assert (TREE_CODE (field) == FIELD_DECL);

  // If the FIELD is from an anonymous aggregate, generate a reference
  // to the anonymous data member, and recur to find FIELD.
  if (ANON_AGGR_TYPE_P (DECL_CONTEXT (field)))
    {
      tree anonymous_field = lookup_anon_field(TREE_TYPE (object), DECL_CONTEXT (field));
      object = component_ref(object, anonymous_field);
    }

  return fold_build3_loc(input_location, COMPONENT_REF,
			 TREE_TYPE (field), object, field, NULL_TREE);
}

// Build a modify expression, with variants for overriding
// the type, and when it's value is not used.

tree
modify_expr(tree dst, tree src)
{
  return fold_build2_loc(input_location, MODIFY_EXPR,
			 TREE_TYPE (dst), dst, src);
}

tree
modify_expr(tree type, tree dst, tree src)
{
  return fold_build2_loc(input_location, MODIFY_EXPR,
			 type, dst, src);
}

tree
vmodify_expr(tree dst, tree src)
{
  return fold_build2_loc(input_location, MODIFY_EXPR,
			 void_type_node, dst, src);
}

tree
build_vinit(tree dst, tree src)
{
  return fold_build2_loc(input_location, INIT_EXPR,
			 void_type_node, dst, src);
}

// Returns true if TYPE contains no actual data, just various
// possible combinations of empty aggregates.

bool
empty_aggregate_p(tree type)
{
  if (!AGGREGATE_TYPE_P (type))
    return false;

  // Want the element type for arrays.
  if (TREE_CODE (type) == ARRAY_TYPE)
    return empty_aggregate_p(TREE_TYPE (type));

  // Recursively check all fields.
  for (tree field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
    {
      if (TREE_CODE (field) == FIELD_DECL
	  && !empty_aggregate_p(TREE_TYPE (field)))
	return false;
    }

  return true;
}

// Return EXP represented as TYPE.

tree
build_nop(tree type, tree exp)
{
  return fold_build1_loc(input_location, NOP_EXPR, type, exp);
}

tree
build_vconvert(tree type, tree exp)
{
  return indirect_ref(type, build_address(exp));
}

// Build a boolean ARG0 op ARG1 expression.

tree
build_boolop(tree_code code, tree arg0, tree arg1)
{
  return fold_build2_loc(input_location, code,
			 bool_type_node, arg0, arg1);
}

// Return a COND_EXPR.  ARG0, ARG1, and ARG2 are the three
// arguments to the conditional expression.

tree
build_condition(tree type, tree arg0, tree arg1, tree arg2)
{
  if (arg1 == void_node)
    arg1 = build_empty_stmt(input_location);

  if (arg2 == void_node)
    arg2 = build_empty_stmt(input_location);

  return fold_build3_loc(input_location, COND_EXPR,
			 type, arg0, arg1, arg2);
}

tree
build_vcondition(tree arg0, tree arg1, tree arg2)
{
  if (arg1 == void_node)
    arg1 = build_empty_stmt(input_location);

  if (arg2 == void_node)
    arg2 = build_empty_stmt(input_location);

  return fold_build3_loc(input_location, COND_EXPR,
			 void_type_node, arg0, arg1, arg2);
}

// Compound ARG0 and ARG1 together.

tree
compound_expr(tree arg0, tree arg1)
{
  return fold_build2_loc(input_location, COMPOUND_EXPR,
			 TREE_TYPE (arg1), arg0, arg1);
}

tree
vcompound_expr(tree arg0, tree arg1)
{
  return fold_build2_loc(input_location, COMPOUND_EXPR,
			 void_type_node, arg0, arg1);
}

// Build a return expression.

tree
return_expr(tree ret)
{
  return fold_build1_loc(input_location, RETURN_EXPR,
			 void_type_node, ret);
}

//

tree
size_mult_expr(tree arg0, tree arg1)
{
  return fold_build2_loc(input_location, MULT_EXPR, size_type_node,
			 d_convert(size_type_node, arg0),
			 d_convert(size_type_node, arg1));

}

// Return the real part of CE, which should be a complex expression.

tree
real_part(tree ce)
{
  return fold_build1_loc(input_location, REALPART_EXPR,
			 TREE_TYPE (TREE_TYPE (ce)), ce);
}

// Return the imaginary part of CE, which should be a complex expression.

tree
imaginary_part(tree ce)
{
  return fold_build1_loc(input_location, IMAGPART_EXPR,
			 TREE_TYPE (TREE_TYPE (ce)), ce);
}

// Build a complex expression of type TYPE using RE and IM.

tree
complex_expr(tree type, tree re, tree im)
{
  return fold_build2_loc(input_location, COMPLEX_EXPR,
			 type, re, im);
}

// Cast EXP (which should be a pointer) to TYPE * and then indirect.  The
// back-end requires this cast in many cases.

tree
indirect_ref(tree type, tree exp)
{
  if (TREE_CODE (TREE_TYPE (exp)) == REFERENCE_TYPE)
    return fold_build1(INDIRECT_REF, type, exp);

  exp = build_nop(build_pointer_type(type), exp);

  return build_deref(exp);
}

// Returns indirect reference of EXP, which must be a pointer type.

tree
build_deref(tree exp)
{
  gcc_assert(POINTER_TYPE_P (TREE_TYPE (exp)));

  if (TREE_CODE (exp) == ADDR_EXPR)
    return TREE_OPERAND (exp, 0);

  return build_fold_indirect_ref(exp);
}

// Builds pointer offset expression PTR[INDEX]

tree
build_array_index(tree ptr, tree index)
{
  tree ptr_type = TREE_TYPE (ptr);
  tree target_type = TREE_TYPE (ptr_type);

  tree type = lang_hooks.types.type_for_size(TYPE_PRECISION (sizetype),
					     TYPE_UNSIGNED (sizetype));

  // array element size
  tree size_exp = size_in_bytes(target_type);

  if (integer_zerop(size_exp))
    {
      // Test for void case...
      if (TYPE_MODE (target_type) == TYPE_MODE (void_type_node))
	index = fold_convert(type, index);
      else
	{
	  // FIXME: should catch this earlier.
	  error("invalid use of incomplete type %qD", TYPE_NAME (target_type));
	  ptr_type = error_mark_node;
	}
    }
  else if (integer_onep(size_exp))
    {
      // ...or byte case -- No need to multiply.
      index = fold_convert(type, index);
    }
  else
    {
      index = d_convert(type, index);
      index = fold_build2(MULT_EXPR, TREE_TYPE (index),
			  index, d_convert(TREE_TYPE (index), size_exp));
      index = fold_convert(type, index);
    }

  // backend will ICE otherwise
  if (error_operand_p(ptr_type))
    return ptr_type;

  if (integer_zerop(index))
    return ptr;

  return fold_build2(POINTER_PLUS_EXPR, ptr_type, ptr, index);
}

// Builds pointer offset expression *(PTR OP INDEX)
// OP could be a plus or minus expression.

tree
build_offset_op(tree_code op, tree ptr, tree index)
{
  gcc_assert(op == MINUS_EXPR || op == PLUS_EXPR);

  tree type = lang_hooks.types.type_for_size(TYPE_PRECISION (sizetype),
					     TYPE_UNSIGNED (sizetype));
  index = fold_convert(type, index);

  if (op == MINUS_EXPR)
    index = fold_build1(NEGATE_EXPR, type, index);

  return fold_build2(POINTER_PLUS_EXPR, TREE_TYPE (ptr), ptr, index);
}

tree
build_offset(tree ptr_node, tree byte_offset)
{
  tree ofs = fold_convert(build_ctype(Type::tsize_t), byte_offset);
  return fold_build2(POINTER_PLUS_EXPR, TREE_TYPE (ptr_node), ptr_node, ofs);
}

tree
build_memref(tree type, tree ptr, tree byte_offset)
{
  tree ofs = fold_convert(type, byte_offset);
  return fold_build2(MEM_REF, type, ptr, ofs);
}


// Create a tree node to set multiple elements to a single value

tree
build_array_set(tree ptr, tree length, tree value)
{
  tree ptrtype = TREE_TYPE (ptr);
  tree lentype = TREE_TYPE (length);

  push_binding_level(level_block);
  push_stmt_list();

  // Build temporary locals for length and ptr, and maybe value.
  tree t = build_local_temp(size_type_node);
  add_stmt(build_vinit(t, length));
  length = t;

  t = build_local_temp(ptrtype);
  add_stmt(build_vinit(t, ptr));
  ptr = t;

  if (d_has_side_effects(value))
    {
      t = build_local_temp(TREE_TYPE (value));
      add_stmt(build_vinit(t, value));
      value = t;
    }

  // Build loop to initialise { .length=length, .ptr=ptr } with value.
  push_stmt_list();

  // Exit logic for the loop.
  //	if (length == 0) break
  t = build_boolop(EQ_EXPR, length, d_convert(lentype, integer_zero_node));
  t = build1(EXIT_EXPR, void_type_node, t);
  add_stmt(t);

  // Assign value to the current pointer position.
  //	*ptr = value
  t = vmodify_expr(build_deref(ptr), value);
  add_stmt(t);

  // Move pointer to next element position.
  //	ptr++;
  tree size = TYPE_SIZE_UNIT (TREE_TYPE (ptrtype));
  t = build2(POSTINCREMENT_EXPR, ptrtype, ptr, d_convert(ptrtype, size));
  add_stmt(t);

  // Decrease loop counter.
  //	length -= 1
  t = build2(POSTDECREMENT_EXPR, lentype, length,
	     d_convert(lentype, integer_one_node));
  add_stmt(t);

  // Pop statements and finish loop.
  tree loop_body = pop_stmt_list();
  add_stmt(build1(LOOP_EXPR, void_type_node, loop_body));

  // Wrap it up into a bind expression.
  tree stmt_list = pop_stmt_list();
  tree block = pop_binding_level();

  return build3(BIND_EXPR, void_type_node,
		BLOCK_VARS (block), stmt_list, block);
}

// Implicitly converts void* T to byte* as D allows { void[] a; &a[3]; }

tree
void_okay_p(tree t)
{
  tree type = TREE_TYPE (t);

  if (VOID_TYPE_P (TREE_TYPE (type)))
    {
      tree totype = build_ctype(Type::tuns8->pointerTo());
      return fold_convert(totype, t);
    }

  return t;
}

// Build an expression of code CODE, data type TYPE, and operands ARG0
// and ARG1. Perform relevant conversions needs for correct code operations.

tree
build_binary_op(tree_code code, tree type, tree arg0, tree arg1)
{
  tree t0 = TREE_TYPE (arg0);
  tree t1 = TREE_TYPE (arg1);
  tree ret = NULL_TREE;

  bool unsignedp = TYPE_UNSIGNED (t0) || TYPE_UNSIGNED (t1);

  // Deal with float mod expressions immediately.
  if (code == FLOAT_MOD_EXPR)
    return build_float_modulus(TREE_TYPE (arg0), arg0, arg1);

  if (POINTER_TYPE_P (t0) && INTEGRAL_TYPE_P (t1))
    return build_nop(type, build_offset_op(code, arg0, arg1));

  if (INTEGRAL_TYPE_P (t0) && POINTER_TYPE_P (t1))
    return build_nop(type, build_offset_op(code, arg1, arg0));

  if (POINTER_TYPE_P (t0) && POINTER_TYPE_P (t1))
    {
      // Need to convert pointers to integers because tree-vrp asserts
      // against (ptr MINUS ptr).
      tree ptrtype = lang_hooks.types.type_for_mode(ptr_mode, TYPE_UNSIGNED (type));
      arg0 = d_convert(ptrtype, arg0);
      arg1 = d_convert(ptrtype, arg1);

      ret = fold_build2(code, ptrtype, arg0, arg1);
    }
  else if (INTEGRAL_TYPE_P (type) && (TYPE_UNSIGNED (type) != unsignedp))
    {
      tree inttype = unsignedp ? d_unsigned_type(type) : d_signed_type(type);
      ret = fold_build2(code, inttype, arg0, arg1);
    }
  else
    {
      // Front-end does not do this conversion and GCC does not
      // always do it right.
      if (COMPLEX_FLOAT_TYPE_P (t0) && !COMPLEX_FLOAT_TYPE_P (t1))
	arg1 = d_convert(t0, arg1);
      else if (COMPLEX_FLOAT_TYPE_P (t1) && !COMPLEX_FLOAT_TYPE_P (t0))
	arg0 = d_convert(t1, arg0);

      ret = fold_build2(code, type, arg0, arg1);
    }

  return d_convert(type, ret);
}

// Build a binary expression of code CODE, assigning the result into E1.

tree
build_binop_assignment(tree_code code, Expression *e1, Expression *e2)
{
  // Skip casts for lhs assignment.
  Expression *e1b = e1;
  while (e1b->op == TOKcast)
    {
      CastExp *ce = (CastExp *) e1b;
      gcc_assert(d_types_same(ce->type, ce->to));
      e1b = ce->e1;
    }

  // Prevent multiple evaluations of LHS, but watch out!
  // The LHS expression could be an assignment, to which
  // it's operation gets lost during gimplification.
  tree lexpr = NULL_TREE;
  tree lhs;

  if (e1b->op == TOKcomma)
    {
      CommaExp *ce = (CommaExp *) e1b;
      lexpr = build_expr(ce->e1);
      lhs = build_expr(ce->e2);
    }
  else
    lhs = build_expr(e1b);

  tree rhs = build_expr(e2);

  // Build assignment expression. Stabilize lhs for assignment.
  lhs = stabilize_reference(lhs);

  rhs = build_binary_op(code, build_ctype(e1->type),
			convert_expr(lhs, e1b->type, e1->type), rhs);

  tree expr = modify_expr(lhs, convert_expr(rhs, e1->type, e1b->type));

  if (lexpr)
    expr = compound_expr(lexpr, expr);

  return expr;
}

// Builds a bounds condition checking that INDEX is between 0 and LEN.
// The condition returns the INDEX if true, or throws a RangeError.
// If INCLUSIVE, we allow INDEX == LEN to return true also.

tree
build_bounds_condition(const Loc& loc, tree index, tree len, bool inclusive)
{
  if (!array_bounds_check())
    return index;

  // Prevent multiple evaluations of the index.
  index = maybe_make_temp(index);

  // Generate INDEX >= LEN && throw RangeError.
  // No need to check whether INDEX >= 0 as the front-end should
  // have already taken care of implicit casts to unsigned.
  tree condition = fold_build2(inclusive ? GT_EXPR : GE_EXPR,
			       bool_type_node, index, len);
  tree boundserr = d_assert_call(loc, LIBCALL_ARRAY_BOUNDS);

  return build_condition(TREE_TYPE (index), condition, boundserr, index);
}

// Returns TRUE if array bounds checking code generation is turned on.

bool
array_bounds_check()
{
  int result = global.params.useArrayBounds;

  if (result == 2)
    return true;

  if (result == 1)
    {
      // For D2 safe functions only
      FuncDeclaration *func = cfun->language->function;
      if (func && func->type->ty == Tfunction)
	{
	  TypeFunction *tf = (TypeFunction *) func->type;
	  if (tf->trust == TRUSTsafe)
	    return true;
	}
    }

  return false;
}

// Builds a BIND_EXPR around BODY for the variables VAR_CHAIN.

tree
bind_expr (tree var_chain, tree body)
{
  // TODO: only handles one var
  gcc_assert (TREE_CHAIN (var_chain) == NULL_TREE);

  if (DECL_INITIAL (var_chain))
    {
      tree ini = build_vinit (var_chain, DECL_INITIAL (var_chain));
      DECL_INITIAL (var_chain) = NULL_TREE;
      body = compound_expr (ini, body);
    }

  return make_temp (build3 (BIND_EXPR, TREE_TYPE (body), var_chain, body, NULL_TREE));
}

// Like compound_expr, but ARG0 or ARG1 might be NULL_TREE.

tree
maybe_compound_expr (tree arg0, tree arg1)
{
  if (arg0 == NULL_TREE)
    return arg1;
  else if (arg1 == NULL_TREE)
    return arg0;
  else
    return compound_expr (arg0, arg1);
}

// Like vcompound_expr, but ARG0 or ARG1 might be NULL_TREE.

tree
maybe_vcompound_expr (tree arg0, tree arg1)
{
  if (arg0 == NULL_TREE)
    return arg1;
  else if (arg1 == NULL_TREE)
    return arg0;
  else
    return vcompound_expr (arg0, arg1);
}

// Returns the TypeFunction class for Type T.
// Assumes T is already ->toBasetype()

TypeFunction *
get_function_type (Type *t)
{
  TypeFunction *tf = NULL;
  if (t->ty == Tpointer)
    t = t->nextOf()->toBasetype();
  if (t->ty == Tfunction)
    tf = (TypeFunction *) t;
  else if (t->ty == Tdelegate)
    tf = (TypeFunction *) ((TypeDelegate *) t)->next;
  return tf;
}

// Returns TRUE if CALLEE is a plain nested function outside the scope of CALLER.
// In which case, CALLEE is being called through an alias that was passed to CALLER.

bool
call_by_alias_p (FuncDeclaration *caller, FuncDeclaration *callee)
{
  if (!callee->isNested())
    return false;

  Dsymbol *dsym = callee;

  while (dsym)
    {
      if (dsym->isTemplateInstance())
	return false;
      else if (dsym->isFuncDeclaration() == caller)
	return false;
      dsym = dsym->toParent();
    }

  return true;
}

// Entry point for call routines.  Builds a function call to FD.
// OBJECT is the 'this' reference passed and ARGS are the arguments to FD.

tree
d_build_call(FuncDeclaration *fd, tree object, Expressions *args)
{
  return d_build_call(get_function_type(fd->type),
		      build_address(fd->toSymbol()->Stree), object, args);
}

// Builds a CALL_EXPR of type TF to CALLABLE. OBJECT holds the 'this' pointer,
// ARGUMENTS are evaluated in left to right order, saved and promoted before passing.

tree
d_build_call(TypeFunction *tf, tree callable, tree object, Expressions *arguments)
{
  tree ctype = TREE_TYPE (callable);
  tree callee = callable;
  tree saved_args = NULL_TREE;

  tree arg_list = NULL_TREE;

  if (POINTER_TYPE_P (ctype))
    ctype = TREE_TYPE (ctype);
  else
    callee = build_address(callable);

  gcc_assert(FUNC_OR_METHOD_TYPE_P (ctype));
  gcc_assert(tf != NULL);
  gcc_assert(tf->ty == Tfunction);

  // Evaluate the callee before calling it.
  if (TREE_SIDE_EFFECTS (callee))
    {
      callee = maybe_make_temp(callee);
      saved_args = callee;
    }

  if (TREE_CODE (ctype) != FUNCTION_TYPE && object == NULL_TREE)
    {
      // Front-end apparently doesn't check this.
      if (TREE_CODE (callable) == FUNCTION_DECL)
	{
	  error("need 'this' to access member %s", IDENTIFIER_POINTER (DECL_NAME (callable)));
	  return error_mark_node;
	}

      // Probably an internal error
      gcc_unreachable();
    }

  // If this is a delegate call or a nested function being called as
  // a delegate, the object should not be NULL.
  if (object != NULL_TREE)
    {
      if (TREE_SIDE_EFFECTS (object))
	{
	  object = maybe_make_temp(object);
	  saved_args = maybe_vcompound_expr(saved_args, object);
	}
      arg_list = build_tree_list(NULL_TREE, object);
    }

  if (arguments)
    {
      // First pass, evaluated expanded tuples in function arguments.
      for (size_t i = 0; i < arguments->dim; ++i)
	{
	Lagain:
	  Expression *arg = (*arguments)[i];
	  gcc_assert(arg->op != TOKtuple);

	  if (arg->op == TOKcomma)
	    {
	      CommaExp *ce = (CommaExp *) arg;
	      tree tce = build_expr(ce->e1);
	      saved_args = maybe_vcompound_expr(saved_args, tce);
	      (*arguments)[i] = ce->e2;
	      goto Lagain;
	    }
	}

      // if _arguments[] is the first argument.
      size_t dvarargs = (tf->linkage == LINKd && tf->varargs == 1);
      size_t nparams = Parameter::dim(tf->parameters);

      // Assumes arguments->dim <= formal_args->dim if (!this->varargs)
      for (size_t i = 0; i < arguments->dim; ++i)
	{
	  Expression *arg = (*arguments)[i];
	  tree targ;

	  if (i < dvarargs)
	    {
	      // The hidden _arguments parameter
	      targ = build_expr(arg);
	    }
	  else if (i - dvarargs < nparams && i >= dvarargs)
	    {
	      // Actual arguments for declared formal arguments
	      Parameter *parg = Parameter::getNth(tf->parameters, i - dvarargs);
	      targ = convert_for_argument(build_expr(arg), arg, parg);
	    }
	  else
	    targ = build_expr(arg);

	  // Don't pass empty aggregates by value.
	  if (empty_aggregate_p(TREE_TYPE (targ)) && !TREE_ADDRESSABLE (targ)
	      && TREE_CODE (targ) != CONSTRUCTOR)
	    {
	      tree t = build_constructor(TREE_TYPE (targ), NULL);
	      targ = build2(COMPOUND_EXPR, TREE_TYPE (t), targ, t);
	    }

	  // Evaluate the argument before passing to the function.
	  // Needed for left to right evaluation.
	  if (tf->linkage == LINKd && TREE_SIDE_EFFECTS (targ))
	    {
	      targ = maybe_make_temp(targ);
	      saved_args = maybe_vcompound_expr(saved_args, targ);
	    }
	  arg_list = chainon(arg_list, build_tree_list(0, targ));
	}
    }

  tree result = d_build_call_list(TREE_TYPE (ctype), callee, arg_list);
  result = expand_intrinsic(result);

  return maybe_compound_expr(saved_args, result);
}

// Builds a call to AssertError or AssertErrorMsg.

tree
d_assert_call (const Loc& loc, LibCall libcall, tree msg)
{
  tree args[3];
  int nargs;

  if (msg != NULL)
    {
      args[0] = msg;
      args[1] = d_array_string (loc.filename ? loc.filename : "");
      args[2] = size_int(loc.linnum);
      nargs = 3;
    }
  else
    {
      args[0] = d_array_string (loc.filename ? loc.filename : "");
      args[1] = size_int(loc.linnum);
      args[2] = NULL_TREE;
      nargs = 2;
    }

  return build_libcall (libcall, nargs, args);
}


// Our internal list of library functions.

static FuncDeclaration *libcall_decls[LIBCALL_count];

// Build and return a function symbol to be used by libcall_decls.

static FuncDeclaration *
get_libcall(const char *name, Type *type, int flags, int nparams, ...)
{
  // Add parameter types, using 'void' as the last parameter type
  // to mean this function accepts a variable list of arguments.
  Parameters *args = new Parameters;
  bool varargs = false;

  va_list ap;
  va_start (ap, nparams);
  for (int i = 0; i < nparams; i++)
    {
      Type *ptype = va_arg(ap, Type *);
      if (ptype != Type::tvoid)
	args->push(new Parameter(0, ptype, NULL, NULL));
      else
	{
	  varargs = true;
	  break;
	}
    }
  va_end(ap);

  // Build extern(C) function.
  FuncDeclaration *decl = FuncDeclaration::genCfunc(args, type, name);

  // Set any attributes on the function, such as malloc or noreturn.
  tree t = decl->toSymbol()->Stree;
  set_call_expr_flags(t, flags);
  DECL_ARTIFICIAL(t) = 1;

  if (varargs)
    {
      TypeFunction *tf = (TypeFunction *) decl->type;
      tf->varargs = true;
    }

  return decl;
}

// Library functions are generated as needed.
// This could probably be changed in the future to be more like GCC builtin
// trees, but we depend on runtime initialisation of front-end types.

FuncDeclaration *
get_libcall(LibCall libcall)
{
  // Build generic AA type void*[void*] for runtime.def
  static Type *AA = NULL;
  if (AA == NULL)
    AA = new TypeAArray(Type::tvoidptr, Type::tvoidptr);

  switch (libcall)
    {
#define DEF_D_RUNTIME(CODE, NAME, PARAMS, TYPE, FLAGS) \
    case LIBCALL_ ## CODE:	\
      libcall_decls[libcall] = get_libcall(NAME, TYPE, FLAGS, PARAMS); \
      break;
#include "runtime.def"
#undef DEF_D_RUNTIME

    default:
      gcc_unreachable();
    }

  return libcall_decls[libcall];
}

// Build call to LIBCALL. N_ARGS is the number of call arguments which are
// specified in as a tree array ARGS.  The caller can force the return type
// of the call to FORCE_TYPE if the library call returns a generic value.

// This does not perform conversions on the arguments.  This allows arbitrary data
// to be passed through varargs without going through the usual conversions.

tree
build_libcall (LibCall libcall, unsigned n_args, tree *args, tree force_type)
{
  // Build the call expression to the runtime function.
  FuncDeclaration *decl = get_libcall(libcall);
  Type *type = decl->type->nextOf();
  tree callee = build_address (decl->toSymbol()->Stree);
  tree arg_list = NULL_TREE;

  for (int i = n_args - 1; i >= 0; i--)
    arg_list = tree_cons (NULL_TREE, args[i], arg_list);

  tree result = d_build_call_list (build_ctype(type), callee, arg_list);

  // Assumes caller knows what it is doing.
  if (force_type != NULL_TREE)
    return convert (force_type, result);

  return result;
}

// Build a call to CALLEE, passing ARGS as arguments.  The expected return
// type is TYPE.  TREE_SIDE_EFFECTS gets set depending on the const/pure
// attributes of the funcion and the SIDE_EFFECTS flags of the arguments.

tree
d_build_call_list (tree type, tree callee, tree args)
{
  int nargs = list_length (args);
  tree *pargs = new tree[nargs];
  for (size_t i = 0; args; args = TREE_CHAIN (args), i++)
    pargs[i] = TREE_VALUE (args);

  return build_call_array (type, callee, nargs, pargs);
}

// Conveniently construct the function arguments for passing
// to the d_build_call_list function.

tree
d_build_call_nary (tree callee, int n_args, ...)
{
  va_list ap;
  tree arg_list = NULL_TREE;
  tree fntype = TREE_TYPE (callee);

  va_start (ap, n_args);
  for (int i = n_args - 1; i >= 0; i--)
    arg_list = tree_cons (NULL_TREE, va_arg (ap, tree), arg_list);
  va_end (ap);

  return d_build_call_list (TREE_TYPE (fntype), build_address (callee), nreverse (arg_list));
}

// List of codes for internally recognised compiler intrinsics.

enum intrinsic_code
{
#define DEF_D_INTRINSIC(CODE, A, N, M, D) INTRINSIC_ ## CODE,
#include "intrinsics.def"
#undef DEF_D_INTRINSIC
  INTRINSIC_LAST
};

// An internal struct used to hold information on D intrinsics.

struct intrinsic_decl
{
  // The DECL_FUNCTION_CODE of this decl.
  intrinsic_code code;

  // The name of the intrinsic.
  const char *name;

  // The module where the intrinsic is located.
  const char *module;

  // The mangled signature decoration of the intrinsic.
  const char *deco;
};

static const intrinsic_decl intrinsic_decls[] =
{
#define DEF_D_INTRINSIC(CODE, ALIAS, NAME, MODULE, DECO) \
    { INTRINSIC_ ## ALIAS, NAME, MODULE, DECO },
#include "intrinsics.def"
#undef DEF_D_INTRINSIC
};

// Call an fold the intrinsic call CALLEE with the argument ARG
// with the built-in function CODE passed.

static tree
expand_intrinsic_op (built_in_function code, tree callee, tree arg)
{
  tree exp = d_build_call_nary (builtin_decl_explicit (code), 1, arg);
  return fold_convert (TREE_TYPE (callee), fold (exp));
}

// Like expand_intrinsic_op, but takes two arguments.

static tree
expand_intrinsic_op2 (built_in_function code, tree callee, tree arg1, tree arg2)
{
  tree exp = d_build_call_nary (builtin_decl_explicit (code), 2, arg1, arg2);
  return fold_convert (TREE_TYPE (callee), fold (exp));
}

// Expand a front-end instrinsic call to bsr whose arguments are ARG.
// The original call expression is held in CALLEE.

static tree
expand_intrinsic_bsr (tree callee, tree arg)
{
  // intrinsic_code bsr gets turned into (size - 1) - count_leading_zeros(arg).
  // %% TODO: The return value is supposed to be undefined if arg is zero.
  tree type = TREE_TYPE (arg);
  tree tsize = build_integer_cst (TREE_INT_CST_LOW (TYPE_SIZE (type)) - 1, type);
  tree exp = expand_intrinsic_op (BUILT_IN_CLZL, callee, arg);

  // Handle int -> long conversions.
  if (TREE_TYPE (exp) != type)
    exp = fold_convert (type, exp);

  exp = fold_build2 (MINUS_EXPR, type, tsize, exp);
  return fold_convert (TREE_TYPE (callee), exp);
}

// Expand a front-end intrinsic call to INTRINSIC, which is either a
// call to bt, btc, btr, or bts.  These intrinsics take two arguments,
// ARG1 and ARG2, and the original call expression is held in CALLEE.

static tree
expand_intrinsic_bt (intrinsic_code intrinsic, tree callee, tree arg1, tree arg2)
{
  tree type = TREE_TYPE (TREE_TYPE (arg1));
  tree exp = build_integer_cst (TREE_INT_CST_LOW (TYPE_SIZE (type)), type);
  tree_code code;
  tree tval;

  // arg1[arg2 / exp]
  arg1 = build_array_index (arg1, fold_build2 (TRUNC_DIV_EXPR, type, arg2, exp));
  arg1 = indirect_ref (type, arg1);

  // mask = 1 << (arg2 % exp);
  arg2 = fold_build2 (TRUNC_MOD_EXPR, type, arg2, exp);
  arg2 = fold_build2 (LSHIFT_EXPR, type, size_one_node, arg2);

  // cond = arg1[arg2 / size] & mask;
  exp = fold_build2 (BIT_AND_EXPR, type, arg1, arg2);

  // cond ? -1 : 0;
  exp = build_condition(TREE_TYPE (callee), d_truthvalue_conversion (exp),
			integer_minus_one_node, integer_zero_node);

  // Update the bit as needed.
  code = (intrinsic == INTRINSIC_BTC) ? BIT_XOR_EXPR :
    (intrinsic == INTRINSIC_BTR) ? BIT_AND_EXPR :
    (intrinsic == INTRINSIC_BTS) ? BIT_IOR_EXPR : ERROR_MARK;
  gcc_assert (code != ERROR_MARK);

  // arg1[arg2 / size] op= mask
  if (intrinsic == INTRINSIC_BTR)
    arg2 = fold_build1 (BIT_NOT_EXPR, TREE_TYPE (arg2), arg2);

  tval = build_local_temp (TREE_TYPE (callee));
  exp = vmodify_expr (tval, exp);
  arg1 = vmodify_expr (arg1, fold_build2 (code, TREE_TYPE (arg1), arg1, arg2));

  return compound_expr (exp, compound_expr (arg1, tval));
}

// Expand a front-end instrinsic call to CODE, which is one of the checkedint
// intrinsics adds, addu, subs, subu, negs, muls, or mulu.
// These intrinsics take three arguments, ARG1, ARG2, and OVERFLOW, with
// exception to negs which takes two arguments, but is re-written as a call
// to subs(0, ARG2, OVERFLOW).
// The original call expression is held in CALLEE.

static tree
expand_intrinsic_arith(built_in_function code, tree callee, tree arg1,
		       tree arg2, tree overflow)
{
  tree result = build_local_temp(TREE_TYPE (callee));

  STRIP_NOPS(overflow);
  overflow = build_deref(overflow);

  // Expands to a __builtin_{add,sub,mul}_overflow.
  tree args[3];
  args[0] = arg1;
  args[1] = arg2;
  args[2] = build_address(result);

  tree fn = builtin_decl_explicit(code);
  tree exp = build_call_array(TREE_TYPE (overflow),
			      build_address(fn), 3, args);

  // Assign returned result to overflow parameter, however if
  // overflow is already true, maintain it's value.
  exp = fold_build2 (BIT_IOR_EXPR, TREE_TYPE (overflow), overflow, exp);
  overflow = vmodify_expr(overflow, exp);

  // Return the value of result.
  return compound_expr(overflow, result);
}

// Expand a front-end built-in call to va_arg, whose arguments are
// ARG1 and optionally ARG2.
// The original call expression is held in CALLEE.

// The cases handled here are:
//	va_arg!T(ap);
//	=>	return (T) VA_ARG_EXP<ap>
//
//	va_arg!T(ap, T arg);
//	=>	return arg = (T) VA_ARG_EXP<ap>;

static tree
expand_intrinsic_vaarg(tree callee, tree arg1, tree arg2)
{
  tree type;

  STRIP_NOPS(arg1);

  if (arg2 == NULL_TREE)
    type = TREE_TYPE(callee);
  else
    {
      STRIP_NOPS(arg2);
      gcc_assert(TREE_CODE(arg2) == ADDR_EXPR);
      arg2 = TREE_OPERAND(arg2, 0);
      type = TREE_TYPE(arg2);
    }

  tree exp = build1(VA_ARG_EXPR, type, arg1);

  if (arg2 != NULL_TREE)
    exp = vmodify_expr(arg2, exp);

  return exp;
}

// Expand a front-end built-in call to va_start, whose arguments are
// ARG1 and ARG2.  The original call expression is held in CALLEE.

static tree
expand_intrinsic_vastart (tree callee, tree arg1, tree arg2)
{
  // The va_list argument should already have its address taken.
  // The second argument, however, is inout and that needs to be
  // fixed to prevent a warning.

  // Could be casting... so need to check type too?
  STRIP_NOPS (arg1);
  STRIP_NOPS (arg2);
  gcc_assert (TREE_CODE (arg1) == ADDR_EXPR && TREE_CODE (arg2) == ADDR_EXPR);

  // Assuming nobody tries to change the return type.
  arg2 = TREE_OPERAND (arg2, 0);
  return expand_intrinsic_op2 (BUILT_IN_VA_START, callee, arg1, arg2);
}

// Checks if DECL is an intrinsic or runtime library function that
// requires special processing.  Marks the generated trees for DECL
// as BUILT_IN_FRONTEND so can be identified later.

void
maybe_set_intrinsic (FuncDeclaration *decl)
{
  if (!decl->ident || decl->builtin == BUILTINyes)
    return;

  // Check if it's a compiler intrinsic.  We only require that any
  // internally recognised intrinsics are declared in a module with
  // an explicit module declaration.
  Module *m = decl->getModule();
  if (!m || !m->md)
    return;

  // Look through all D intrinsics.
  TemplateInstance *ti = decl->isInstantiated();
  TemplateDeclaration *td = ti ? ti->tempdecl->isTemplateDeclaration() : NULL;
  const char *tname = decl->ident->string;
  const char *tmodule = m->md->toChars();
  const char *tdeco = decl->type->deco;

  for (size_t i = 0; i < (int) INTRINSIC_LAST; i++)
    {
      if (strcmp (intrinsic_decls[i].name, tname) != 0
	  || strcmp (intrinsic_decls[i].module, tmodule) != 0)
	continue;

      if (td && td->onemember)
	{
	  FuncDeclaration *fd = td->onemember->isFuncDeclaration();
	  if (fd != NULL
	      && strcmp (fd->type->toChars(), intrinsic_decls[i].deco) == 0)
	    goto Lfound;
	}
      else if (strcmp (intrinsic_decls[i].deco, tdeco) == 0)
	{
	Lfound:
	  intrinsic_code code = intrinsic_decls[i].code;

	  if (decl->csym == NULL)
	    {
	      // Store a stub BUILT_IN_FRONTEND decl.
	      decl->csym = new Symbol();
	      decl->csym->Stree = build_decl (BUILTINS_LOCATION, FUNCTION_DECL,
					      NULL_TREE, NULL_TREE);
	      DECL_NAME (decl->csym->Stree) = get_identifier (tname);
	      TREE_TYPE (decl->csym->Stree) = build_ctype(decl->type);
	      d_keep (decl->csym->Stree);
	    }

	  DECL_BUILT_IN_CLASS (decl->csym->Stree) = BUILT_IN_FRONTEND;
	  DECL_FUNCTION_CODE (decl->csym->Stree) = (built_in_function) code;
	  decl->builtin = BUILTINyes;
	  break;
	}
    }
}

//
tree
expand_volatile_load(tree arg1)
{
  tree type = TREE_TYPE (arg1);
  gcc_assert (POINTER_TYPE_P (type));

  tree tvolatile = build_qualified_type(TREE_TYPE (arg1), TYPE_QUAL_VOLATILE);
  tree result = indirect_ref(tvolatile, arg1);
  TREE_THIS_VOLATILE (result) = 1;

  return result;
}

tree
expand_volatile_store(tree arg1, tree arg2)
{
  tree tvolatile = build_qualified_type(TREE_TYPE (arg2), TYPE_QUAL_VOLATILE);
  tree result = indirect_ref(tvolatile, arg1);
  TREE_THIS_VOLATILE (result) = 1;

  return modify_expr(tvolatile, result, arg2);
}

// If CALLEXP is a BUILT_IN_FRONTEND, expand and return inlined
// compiler generated instructions. Most map onto GCC builtins,
// others require a little extra work around them.

tree
expand_intrinsic(tree callexp)
{
  tree callee = CALL_EXPR_FN (callexp);

  if (TREE_CODE (callee) == ADDR_EXPR)
    callee = TREE_OPERAND (callee, 0);

  if (TREE_CODE (callee) == FUNCTION_DECL
      && DECL_BUILT_IN_CLASS (callee) == BUILT_IN_FRONTEND)
    {
      intrinsic_code intrinsic = (intrinsic_code) DECL_FUNCTION_CODE (callee);
      tree op1, op2, op3;
      tree type;

      switch (intrinsic)
	{
	case INTRINSIC_BSF:
	  // Builtin count_trailing_zeros matches behaviour of bsf
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_CTZL, callexp, op1);

	case INTRINSIC_BSR:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_bsr(callexp, op1);

	case INTRINSIC_BTC:
	case INTRINSIC_BTR:
	case INTRINSIC_BTS:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  return expand_intrinsic_bt(intrinsic, callexp, op1, op2);

	case INTRINSIC_BSWAP:
	  // Backend provides builtin bswap32.
	  // Assumes first argument and return type is uint.
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_BSWAP32, callexp, op1);

	  // Math intrinsics just map to their GCC equivalents.
	case INTRINSIC_COS:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_COSL, callexp, op1);

	case INTRINSIC_SIN:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_SINL, callexp, op1);

	case INTRINSIC_RNDTOL:
	  // Not sure if llroundl stands as a good replacement for the
	  // expected behaviour of rndtol.
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_LLROUNDL, callexp, op1);

	case INTRINSIC_SQRT:
	case INTRINSIC_SQRTF:
	case INTRINSIC_SQRTL:
	  // Have float, double and real variants of sqrt.
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  type = TYPE_MAIN_VARIANT (TREE_TYPE (op1));
	  // op1 is an integral type - use double precision.
	  if (INTEGRAL_TYPE_P (type))
	    op1 = convert(double_type_node, op1);

	  if (intrinsic == INTRINSIC_SQRT)
	    return expand_intrinsic_op(BUILT_IN_SQRT, callexp, op1);
	  else if (intrinsic == INTRINSIC_SQRTF)
	    return expand_intrinsic_op(BUILT_IN_SQRTF, callexp, op1);
	  else if (intrinsic == INTRINSIC_SQRTL)
	    return expand_intrinsic_op(BUILT_IN_SQRTL, callexp, op1);

	  gcc_unreachable();
	  break;

	case INTRINSIC_LDEXP:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  return expand_intrinsic_op2(BUILT_IN_LDEXPL, callexp, op1, op2);

	case INTRINSIC_FABS:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_FABSL, callexp, op1);

	case INTRINSIC_RINT:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_op(BUILT_IN_RINTL, callexp, op1);

	case INTRINSIC_VA_ARG:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  return expand_intrinsic_vaarg(callexp, op1, op2);

	case INTRINSIC_C_VA_ARG:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_intrinsic_vaarg(callexp, op1, NULL_TREE);

	case INTRINSIC_VASTART:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  return expand_intrinsic_vastart(callexp, op1, op2);

	case INTRINSIC_ADDS:
	case INTRINSIC_ADDSL:
	case INTRINSIC_ADDU:
	case INTRINSIC_ADDUL:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  op3 = CALL_EXPR_ARG (callexp, 2);
	  return expand_intrinsic_arith(BUILT_IN_ADD_OVERFLOW,
					callexp, op1, op2, op3);

	case INTRINSIC_SUBS:
	case INTRINSIC_SUBSL:
	case INTRINSIC_SUBU:
	case INTRINSIC_SUBUL:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  op3 = CALL_EXPR_ARG (callexp, 2);
	  return expand_intrinsic_arith(BUILT_IN_SUB_OVERFLOW,
					callexp, op1, op2, op3);

	case INTRINSIC_MULS:
	case INTRINSIC_MULSL:
	case INTRINSIC_MULU:
	case INTRINSIC_MULUL:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  op3 = CALL_EXPR_ARG (callexp, 2);
	  return expand_intrinsic_arith(BUILT_IN_MUL_OVERFLOW,
					callexp, op1, op2, op3);

	case INTRINSIC_NEGS:
	case INTRINSIC_NEGSL:
	  op1 = fold_convert (TREE_TYPE (callexp), integer_zero_node);
	  op2 = CALL_EXPR_ARG (callexp, 0);
	  op3 = CALL_EXPR_ARG (callexp, 1);
	  return expand_intrinsic_arith(BUILT_IN_SUB_OVERFLOW,
					callexp, op1, op2, op3);

	case INTRINSIC_VLOAD:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  return expand_volatile_load(op1);

	case INTRINSIC_VSTORE:
	  op1 = CALL_EXPR_ARG (callexp, 0);
	  op2 = CALL_EXPR_ARG (callexp, 1);
	  return expand_volatile_store(op1, op2);

	default:
	  gcc_unreachable();
	}
    }

  return callexp;
}

// Build and return the correct call to fmod depending on TYPE.
// ARG0 and ARG1 are the arguments pass to the function.

tree
build_float_modulus (tree type, tree arg0, tree arg1)
{
  tree fmodfn = NULL_TREE;
  tree basetype = type;

  if (COMPLEX_FLOAT_TYPE_P (basetype))
    basetype = TREE_TYPE (basetype);

  if (TYPE_MAIN_VARIANT (basetype) == double_type_node
      || TYPE_MAIN_VARIANT (basetype) == idouble_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMOD);
  else if (TYPE_MAIN_VARIANT (basetype) == float_type_node
	   || TYPE_MAIN_VARIANT (basetype) == ifloat_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMODF);
  else if (TYPE_MAIN_VARIANT (basetype) == long_double_type_node
	   || TYPE_MAIN_VARIANT (basetype) == ireal_type_node)
    fmodfn = builtin_decl_explicit (BUILT_IN_FMODL);

  if (!fmodfn)
    {
      // %qT pretty prints the tree type.
      error ("tried to perform floating-point modulo division on %qT", type);
      return error_mark_node;
    }

  if (COMPLEX_FLOAT_TYPE_P (type))
    {
      tree re = d_build_call_nary(fmodfn, 2, real_part(arg0), arg1);
      tree im = d_build_call_nary(fmodfn, 2, imaginary_part(arg0), arg1);

      return complex_expr(type, re, im);
    }

  if (SCALAR_FLOAT_TYPE_P (type))
    return d_build_call_nary (fmodfn, 2, arg0, arg1);

  // Should have caught this above.
  gcc_unreachable();
}

// Returns typeinfo reference for type T.

tree
build_typeinfo (Type *t)
{
  tree tinfo = build_expr(getTypeInfo(t, NULL));
  gcc_assert(POINTER_TYPE_P (TREE_TYPE (tinfo)));
  return tinfo;
}

// Check that a new jump at FROM to a label at TO is OK.

void
check_goto(Statement *from, Statement *to)
{
  d_label_entry *ent = cfun->language->labels->get(to);
  gcc_assert(ent != NULL);

  // If the label hasn't been defined yet, defer checking.
  if (! DECL_INITIAL (ent->label))
    {
      d_label_use_entry *fwdref = ggc_alloc<d_label_use_entry>();
      fwdref->level = current_binding_level;
      fwdref->statement = from;
      fwdref->next = ent->fwdrefs;
      ent->fwdrefs = fwdref;
      return;
    }

  if (ent->in_try_scope)
    from->error("cannot goto into try block");
  else if (ent->in_catch_scope)
    from->error("cannot goto into catch block");
}

// Check that a previously seen jumps to a newly defined label is OK.

static void
check_previous_goto(Statement *s, d_label_use_entry *fwdref)
{
  for (binding_level *b = current_binding_level; b ; b = b->level_chain)
    {
      if (b == fwdref->level)
	break;

      if (b->kind == level_try || b->kind == level_catch)
	{
	  if (s->isLabelStatement())
	    {
	      if (b->kind == level_try)
		fwdref->statement->error("cannot goto into try block");
	      else
		fwdref->statement->error("cannot goto into catch block");
	    }
	  else if (s->isCaseStatement())
	    s->error("case cannot be in different try block level from switch");
	  else if (s->isDefaultStatement())
	    s->error("default cannot be in different try block level from switch");
	  else
	    gcc_unreachable();
	}
    }
}

// Get or build LABEL_DECL using the IDENT and statement block S given.

tree
lookup_label(Statement *s, Identifier *ident)
{
  // You can't use labels at global scope.
  if (cfun == NULL)
    {
      error("label %s referenced outside of any function",
	    ident ? ident->string : "(unnamed)");
      return NULL_TREE;
    }

  // Create the label htab for the function on demand.
  if (!cfun->language->labels)
    cfun->language->labels = hash_map<Statement *, d_label_entry>::create_ggc(13);

  d_label_entry *ent = cfun->language->labels->get(s);
  if (ent != NULL)
    return ent->label;
  else
    {
      tree name = ident ? get_identifier(ident->string) : NULL_TREE;
      tree decl = build_decl(input_location, LABEL_DECL, name, void_type_node);
      DECL_CONTEXT (decl) = current_function_decl;
      DECL_MODE (decl) = VOIDmode;

      // Create new empty slot.
      ent = ggc_cleared_alloc<d_label_entry>();
      ent->statement = s;
      ent->label = decl;

      bool existed = cfun->language->labels->put(s, *ent);
      gcc_assert(!existed);

      return decl;
    }
}

// Get the LABEL_DECL to represent a break or continue for the
// statement S given.  BC indicates which.

tree
lookup_bc_label(Statement *s, bc_kind bc)
{
  tree vec = lookup_label(s);

  // The break and continue labels are put into a TREE_VEC.
  if (TREE_CODE (vec) == LABEL_DECL)
    {
      d_label_entry *ent = cfun->language->labels->get(s);
      gcc_assert(ent != NULL);

      vec = make_tree_vec(2);
      TREE_VEC_ELT (vec, bc_break) = ent->label;

      // Build the continue label.
      tree label = build_decl(input_location, LABEL_DECL,
			      NULL_TREE, void_type_node);
      DECL_CONTEXT (label) = current_function_decl;
      DECL_MODE (label) = VOIDmode;
      TREE_VEC_ELT (vec, bc_continue) = label;

      ent->label = vec;
      ent->bc_label = true;
    }

  return TREE_VEC_ELT (vec, bc);
}

// Define a label, specifying the location in the source file.
// Return the LABEL_DECL node for the label.

tree
define_label(Statement *s, Identifier *ident)
{
  tree label = lookup_label(s, ident);
  gcc_assert(DECL_INITIAL (label) == NULL_TREE);

  d_label_entry *ent = cfun->language->labels->get(s);
  gcc_assert(ent != NULL);

  // Mark label as having been defined.
  DECL_INITIAL (label) = error_mark_node;

  // Not setting this doesn't seem to cause problems (unlike VAR_DECLs).
  if (s->loc.filename)
    set_decl_location (label, s->loc);

  ent->level = current_binding_level;

  for (d_label_use_entry *ref = ent->fwdrefs; ref ; ref = ref->next)
    check_previous_goto(ent->statement, ref);
  ent->fwdrefs = NULL;

  return label;
}

// Build a function type whose first argument is a pointer to BASETYPE,
// which is to be used for the 'vthis' parameter for TYPE.
// The base type may be a record for member functions, or a void for
// nested functions and delegates.

tree
build_vthis_type(tree basetype, tree type)
{
  gcc_assert (TREE_CODE (type) == FUNCTION_TYPE);

  tree argtypes = tree_cons(NULL_TREE, build_pointer_type(basetype),
			    TYPE_ARG_TYPES (type));
  tree fntype = build_function_type(TREE_TYPE (type), argtypes);

  if (RECORD_OR_UNION_TYPE_P (basetype))
    TYPE_METHOD_BASETYPE (fntype) = TYPE_MAIN_VARIANT (basetype);
  else
    gcc_assert(VOID_TYPE_P (basetype));

  return fntype;
}

// If SYM is a nested function, return the static chain to be
// used when calling that function from the current function.

// If SYM is a nested class or struct, return the static chain
// to be used when creating an instance of the class from CFUN.

tree
get_frame_for_symbol (Dsymbol *sym)
{
  FuncDeclaration *func = cfun ? cfun->language->function : NULL;
  FuncDeclaration *thisfd = sym->isFuncDeclaration();
  FuncDeclaration *parentfd = NULL;

  if (thisfd != NULL)
    {
      // Check that the nested function is properly defined.
      if (!thisfd->fbody)
	{
	  // Should instead error on line that references 'thisfd'.
	  thisfd->error ("nested function missing body");
	  return null_pointer_node;
	}

      // Special case for __ensure and __require.
      if (thisfd->ident == Id::ensure || thisfd->ident == Id::require)
	parentfd = func;
      else
	parentfd = thisfd->toParent2()->isFuncDeclaration();
    }
  else
    {
      // It's a class (or struct). NewExp codegen has already determined its
      // outer scope is not another class, so it must be a function.
      while (sym && !sym->isFuncDeclaration())
	sym = sym->toParent2();

      parentfd = (FuncDeclaration *) sym;
    }

  gcc_assert (parentfd != NULL);

  if (func != parentfd)
    {
      // If no frame pointer for this function
      if (!func->vthis)
	{
	  sym->error ("is a nested function and cannot be accessed from %s", func->toChars());
	  return null_pointer_node;
	}

      // Make sure we can get the frame pointer to the outer function.
      // Go up each nesting level until we find the enclosing function.
      Dsymbol *dsym = func;

      while (thisfd != dsym)
	{
	  // Check if enclosing function is a function.
	  FuncDeclaration *fd = dsym->isFuncDeclaration();

	  if (fd != NULL)
	    {
	      if (parentfd == fd->toParent2())
		break;

	      gcc_assert (fd->isNested() || fd->vthis);
	      dsym = dsym->toParent2();
	      continue;
	    }

	  // Check if enclosed by an aggregate. That means the current
	  // function must be a member function of that aggregate.
	  AggregateDeclaration *ad = dsym->isAggregateDeclaration();

	  if (ad == NULL)
	    goto Lnoframe;
	  if (ad->isClassDeclaration() && parentfd == ad->toParent2())
	    break;
	  if (ad->isStructDeclaration() && parentfd == ad->toParent2())
	    break;

	  if (!ad->isNested() || !ad->vthis)
	    {
	    Lnoframe:
	      func->error ("cannot get frame pointer to %s", sym->toChars());
	      return null_pointer_node;
	    }

	  dsym = dsym->toParent2();
	}
    }

  FuncFrameInfo *ffo = get_frameinfo (parentfd);
  if (ffo->creates_frame || ffo->static_chain)
    return get_framedecl (func, parentfd);

  return null_pointer_node;
}

// Return the parent function of a nested class CD.

static FuncDeclaration *
d_nested_class (ClassDeclaration *cd)
{
  FuncDeclaration *fd = NULL;
  while (cd && cd->isNested())
    {
      Dsymbol *dsym = cd->toParent2();
      if ((fd = dsym->isFuncDeclaration()))
	return fd;
      else
	cd = dsym->isClassDeclaration();
    }
  return NULL;
}

// Return the parent function of a nested struct SD.

static FuncDeclaration *
d_nested_struct (StructDeclaration *sd)
{
  FuncDeclaration *fd = NULL;
  while (sd && sd->isNested())
    {
      Dsymbol *dsym = sd->toParent2();
      if ((fd = dsym->isFuncDeclaration()))
	return fd;
      else
	sd = dsym->isStructDeclaration();
    }
  return NULL;
}


// Starting from the current function FUNC, try to find a suitable value of
// 'this' in nested function instances.  A suitable 'this' value is an
// instance of OCD or a class that has OCD as a base.

static tree
find_this_tree(ClassDeclaration *ocd)
{
  FuncDeclaration *func = cfun ? cfun->language->function : NULL;

  while (func)
    {
      AggregateDeclaration *ad = func->isThis();
      ClassDeclaration *cd = ad ? ad->isClassDeclaration() : NULL;

      if (cd != NULL)
	{
	  if (ocd == cd)
	    return get_decl_tree(func->vthis);
	  else if (ocd->isBaseOf(cd, NULL))
	    return convert_expr(get_decl_tree(func->vthis), cd->type, ocd->type);

	  func = d_nested_class(cd);
	}
      else
	{
	  if (func->isNested())
	    {
	      func = func->toParent2()->isFuncDeclaration();
	      continue;
	    }

	  func = NULL;
	}
    }

  return NULL_TREE;
}

// Retrieve the outer class/struct 'this' value of DECL from
// the current function.

tree
build_vthis(AggregateDeclaration *decl)
{
  ClassDeclaration *cd = decl->isClassDeclaration();
  StructDeclaration *sd = decl->isStructDeclaration();

  // If an aggregate nested in a function has no methods and there are no
  // other nested functions, any static chain created here will never be
  // translated.  Use a null pointer for the link in this case.
  tree vthis_value = null_pointer_node;

  if (cd != NULL || sd != NULL)
    {
      Dsymbol *outer = decl->toParent2();

      // If the parent is a templated struct, the outer context is instead
      // the enclosing symbol of where the instantiation happened.
      if (outer->isStructDeclaration())
	{
	  gcc_assert(outer->parent && outer->parent->isTemplateInstance());
	  outer = ((TemplateInstance *) outer->parent)->enclosing;
	}

      // For outer classes, get a suitable 'this' value.
      // For outer functions, get a suitable frame/closure pointer.
      ClassDeclaration *cdo = outer->isClassDeclaration();
      FuncDeclaration *fdo = outer->isFuncDeclaration();

      if (cdo)
	{
	  vthis_value = find_this_tree(cdo);
	  gcc_assert(vthis_value != NULL_TREE);
	}
      else if (fdo)
	{
	  FuncFrameInfo *ffo = get_frameinfo(fdo);
	  if (ffo->creates_frame || ffo->static_chain
	      || fdo->hasNestedFrameRefs())
	    vthis_value = get_frame_for_symbol(decl);
	  else if (cd != NULL)
	    {
	      // Classes nested in methods are allowed to access any outer
	      // class fields, use the function chain in this case.
	      if (fdo->vthis && fdo->vthis->type != Type::tvoidptr)
		vthis_value = get_decl_tree(fdo->vthis);
	    }
	}
      else
	gcc_unreachable();
    }

  return vthis_value;
}

tree
build_frame_type (FuncDeclaration *func)
{
  FuncFrameInfo *ffi = get_frameinfo (func);

  if (ffi->frame_rec != NULL_TREE)
    return ffi->frame_rec;

  tree frame_rec_type = make_node (RECORD_TYPE);
  char *name = concat (ffi->is_closure ? "CLOSURE." : "FRAME.",
		       func->toPrettyChars(), NULL);
  TYPE_NAME (frame_rec_type) = get_identifier (name);
  free (name);

  tree ptr_field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
			       get_identifier ("__chain"), ptr_type_node);
  DECL_FIELD_CONTEXT (ptr_field) = frame_rec_type;
  TYPE_READONLY (frame_rec_type) = 1;

  tree fields = chainon (NULL_TREE, ptr_field);

  if (!ffi->is_closure)
    {
      // __ensure and __require never becomes a closure, but could still be referencing
      // parameters of the calling function.  So we add all parameters as nested refs.
      // This is written as such so that all parameters appear at the front of the frame
      // so that overriding methods match the same layout when inheriting a contract.
      if ((global.params.useIn && func->frequire) || (global.params.useOut && func->fensure))
	{
	  for (size_t i = 0; func->parameters && i < func->parameters->dim; i++)
	    {
	      VarDeclaration *v = (*func->parameters)[i];
	      // Remove if already in closureVars so can push to front.
	      for (size_t j = i; j < func->closureVars.dim; j++)
		{
		  Dsymbol *s = func->closureVars[j];
		  if (s == v)
		    {
		      func->closureVars.remove (j);
		      break;
		    }
		}
	      func->closureVars.insert (i, v);
	    }

	  // Also add hidden 'this' to outer context.
	  if (func->vthis)
	    {
	      for (size_t i = 0; i < func->closureVars.dim; i++)
		{
		  Dsymbol *s = func->closureVars[i];
		  if (s == func->vthis)
		    {
		      func->closureVars.remove (i);
		      break;
		    }
		}
	      func->closureVars.insert (0, func->vthis);
	    }
	}
    }

  for (size_t i = 0; i < func->closureVars.dim; i++)
    {
      VarDeclaration *v = func->closureVars[i];
      Symbol *s = v->toSymbol();
      tree field = build_decl (BUILTINS_LOCATION, FIELD_DECL,
			       v->ident ? get_identifier (v->ident->string) : NULL_TREE,
			       declaration_type (v));
      s->SframeField = field;
      set_decl_location (field, v);
      DECL_FIELD_CONTEXT (field) = frame_rec_type;
      fields = chainon (fields, field);
      TREE_USED (s->Stree) = 1;

      // Can't do nrvo if the variable is put in a frame.
      if (func->nrvo_can && func->nrvo_var == v)
	func->nrvo_can = 0;

      // Because the value needs to survive the end of the scope.
      if (ffi->is_closure && v->needsAutoDtor())
	v->error("has scoped destruction, cannot build closure");
    }

  TYPE_FIELDS (frame_rec_type) = fields;
  layout_type (frame_rec_type);
  d_keep (frame_rec_type);

  return frame_rec_type;
}

// Closures are implemented by taking the local variables that
// need to survive the scope of the function, and copying them
// into a gc allocated chuck of memory. That chunk, called the
// closure here, is inserted into the linked list of stack
// frames instead of the usual stack frame.

// If a closure is not required, but FD still needs a frame to lower
// nested refs, then instead build custom static chain decl on stack.

void
build_closure(FuncDeclaration *fd)
{
  FuncFrameInfo *ffi = get_frameinfo(fd);

  if (!ffi->creates_frame)
    return;

  tree type = build_frame_type(fd);
  gcc_assert(COMPLETE_TYPE_P(type));

  tree decl, decl_ref;

  if (ffi->is_closure)
    {
      decl = build_local_temp(build_pointer_type(type));
      DECL_NAME(decl) = get_identifier("__closptr");
      decl_ref = build_deref(decl);

      // Allocate memory for closure.
      tree arg = convert(build_ctype(Type::tsize_t), TYPE_SIZE_UNIT(type));
      tree init = build_libcall(LIBCALL_ALLOCMEMORY, 1, &arg);

      DECL_INITIAL(decl) = build_nop(TREE_TYPE(decl), init);
    }
  else
    {
      decl = build_local_temp(type);
      DECL_NAME(decl) = get_identifier("__frame");
      decl_ref = decl;
    }

  DECL_IGNORED_P(decl) = 0;
  expand_decl(decl);

  // Set the first entry to the parent closure/frame, if any.
  tree chain_field = component_ref(decl_ref, TYPE_FIELDS(type));
  tree chain_expr = vmodify_expr(chain_field, cfun->language->static_chain);
  add_stmt(chain_expr);

  // Copy parameters that are referenced nonlocally.
  for (size_t i = 0; i < fd->closureVars.dim; i++)
    {
      VarDeclaration *v = fd->closureVars[i];

      if (!v->isParameter())
	continue;

      Symbol *vsym = v->toSymbol();

      tree field = component_ref (decl_ref, vsym->SframeField);
      tree expr = vmodify_expr (field, vsym->Stree);
      add_stmt(expr);
    }

  if (!ffi->is_closure)
    decl = build_address (decl);

  cfun->language->static_chain = decl;
}

// Return the frame of FD.  This could be a static chain or a closure
// passed via the hidden 'this' pointer.

FuncFrameInfo *
get_frameinfo(FuncDeclaration *fd)
{
  Symbol *fds = fd->toSymbol();
  if (fds->frameInfo)
    return fds->frameInfo;

  FuncFrameInfo *ffi = new FuncFrameInfo;
  ffi->creates_frame = false;
  ffi->static_chain = false;
  ffi->is_closure = false;
  ffi->frame_rec = NULL_TREE;

  fds->frameInfo = ffi;

  // Nested functions, or functions with nested refs must create
  // a static frame for local variables to be referenced from.
  if (fd->closureVars.dim != 0)
    ffi->creates_frame = true;

  if (fd->vthis && fd->vthis->type == Type::tvoidptr)
    ffi->creates_frame = true;

  // Functions with In/Out contracts pass parameters to nested frame.
  if (fd->fensure || fd->frequire)
    ffi->creates_frame = true;

  // D2 maybe setup closure instead.
  if (fd->needsClosure())
    {
      ffi->creates_frame = true;
      ffi->is_closure = true;
    }
  else if (fd->closureVars.dim == 0)
    {
      /* If fd is nested (deeply) in a function that creates a closure,
	 then fd inherits that closure via hidden vthis pointer, and
	 doesn't create a stack frame at all.  */
      FuncDeclaration *ff = fd;

      while (ff)
	{
	  FuncFrameInfo *ffo = get_frameinfo (ff);
	  AggregateDeclaration *ad;

	  if (ff != fd && ffo->creates_frame)
	    {
	      gcc_assert (ffo->frame_rec);
	      ffi->creates_frame = false;
	      ffi->static_chain = true;
	      ffi->is_closure = ffo->is_closure;
	      gcc_assert (COMPLETE_TYPE_P (ffo->frame_rec));
	      ffi->frame_rec = ffo->frame_rec;
	      break;
	    }

	  // Stop looking if no frame pointer for this function.
	  if (ff->vthis == NULL)
	    break;

	  ad = ff->isThis();
	  if (ad && ad->isNested())
	    {
	      while (ad->isNested())
		{
		  Dsymbol *d = ad->toParent2();
		  ad = d->isAggregateDeclaration();
		  ff = d->isFuncDeclaration();

		  if (ad == NULL)
		    break;
		}
	    }
	  else
	    ff = ff->toParent2()->isFuncDeclaration();
	}
    }

  // Build type now as may be referenced from another module.
  if (ffi->creates_frame)
    ffi->frame_rec = build_frame_type (fd);

  return ffi;
}

// Return a pointer to the frame/closure block of OUTER
// so can be accessed from the function INNER.

tree
get_framedecl (FuncDeclaration *inner, FuncDeclaration *outer)
{
  tree result = cfun->language->static_chain;
  FuncDeclaration *fd = inner;

  while (fd && fd != outer)
    {
      AggregateDeclaration *ad;
      ClassDeclaration *cd;
      StructDeclaration *sd;

      // Parent frame link is the first field.
      if (get_frameinfo (fd)->creates_frame)
	result = indirect_ref (ptr_type_node, result);

      if (fd->isNested())
	fd = fd->toParent2()->isFuncDeclaration();
      // The frame/closure record always points to the outer function's
      // frame, even if there are intervening nested classes or structs.
      // So, we can just skip over these...
      else if ((ad = fd->isThis()) && (cd = ad->isClassDeclaration()))
	fd = d_nested_class (cd);
      else if ((ad = fd->isThis()) && (sd = ad->isStructDeclaration()))
	fd = d_nested_struct (sd);
      else
	break;
    }

  // Go get our frame record.
  gcc_assert (fd == outer);
  tree frame_rec = get_frameinfo (outer)->frame_rec;

  if (frame_rec != NULL_TREE)
    {
      result = build_nop (build_pointer_type (frame_rec), result);
      return result;
    }
  else
    {
      inner->error ("forward reference to frame of %s", outer->toChars());
      return null_pointer_node;
    }
}


// For all decls in the FIELDS chain, adjust their field offset by OFFSET.
// This is done as the frontend puts fields into the outer struct, and so
// their offset is from the beginning of the aggregate.
// We want the offset to be from the beginning of the anonymous aggregate.

static void
fixup_anonymous_offset(tree fields, tree offset)
{
  while (fields != NULL_TREE)
    {
      // Traverse all nested anonymous aggregates to update their offset.
      // Set the anonymous decl offset to it's first member.
      tree ftype = TREE_TYPE (fields);
      if (TYPE_NAME (ftype) && anon_aggrname_p(TYPE_IDENTIFIER (ftype)))
	{
	  tree vfields = TYPE_FIELDS (ftype);
	  fixup_anonymous_offset(vfields, offset);
	  DECL_FIELD_OFFSET (fields) = DECL_FIELD_OFFSET (vfields);
	}
      else
	{
	  tree voffset = DECL_FIELD_OFFSET (fields);
	  DECL_FIELD_OFFSET (fields) = size_binop(MINUS_EXPR, voffset, offset);
	}

      fields = DECL_CHAIN (fields);
    }
}

// Iterate over all MEMBERS of an aggregate, and add them as fields to CONTEXT.
// If INHERITED_P is true, then the members derive from a base class.
// Returns the number of fields found.

static size_t
layout_aggregate_members(Dsymbols *members, tree context, bool inherited_p)
{
  size_t fields = 0;

  for (size_t i = 0; i < members->dim; i++)
    {
      Dsymbol *sym = (*members)[i];
      VarDeclaration *var = sym->isVarDeclaration();
      if (var != NULL)
	{
	  // Skip fields that have already been added.
	  if (!inherited_p && var->csym != NULL)
	    continue;

	  // If this variable was really a tuple, add all tuple fields.
	  if (var->aliassym)
	    {
	      TupleDeclaration *td = var->aliassym->isTupleDeclaration();
	      Dsymbols tmembers;
	      // No other way to coerce the underlying type out of the tuple.
	      // Runtime checks should have already been done by the frontend.
	      for (size_t j = 0; j < td->objects->dim; j++)
		{
		  RootObject *ro = (*td->objects)[j];
		  gcc_assert(ro->dyncast() == DYNCAST_EXPRESSION);
		  Expression *e = (Expression *) ro;
		  gcc_assert(e->op == TOKdsymbol);
		  DsymbolExp *se = (DsymbolExp *) e;

		  tmembers.push(se->s);
		}

	      fields += layout_aggregate_members(&tmembers, context, inherited_p);
	      continue;
	    }

	  // Insert the field declaration at it's given offset.
	  if (var->isField())
	    {
	      tree ident = var->ident ? get_identifier(var->ident->string) : NULL_TREE;
	      tree field = build_decl(UNKNOWN_LOCATION, FIELD_DECL, ident,
				      declaration_type(var));
	      DECL_ARTIFICIAL (field) = inherited_p;
	      DECL_IGNORED_P (field) = inherited_p;
	      insert_aggregate_field(var->loc, context, field, var->offset);

	      // Because the front-end shares field decls across classes, don't
	      // create the corresponding backend symbol unless we are adding
	      // it to the aggregate it is defined in.
	      if (!inherited_p)
		{
		  var->csym = new Symbol();
		  var->csym->Stree = field;
		}

	      fields += 1;
	      continue;
	    }
	}

      // Anonymous struct/union are treated as flat attributes by the front-end.
      // However, we need to keep the record layout intact when building the type.
      AnonDeclaration *ad = sym->isAnonDeclaration();
      if (ad != NULL)
	{
	  // Use a counter to create anonymous type names.
	  static int anon_cnt = 0;
	  char buf[32];
	  sprintf(buf, anon_aggrname_format(), anon_cnt++);

	  tree ident = get_identifier(buf);
	  tree type = make_node(ad->isunion ? UNION_TYPE : RECORD_TYPE);
	  ANON_AGGR_TYPE_P (type) = 1;
	  d_keep(type);

	  // Build the type declaration.
	  tree decl = build_decl(UNKNOWN_LOCATION, TYPE_DECL, ident, type);
	  DECL_CONTEXT (decl) = context;
	  DECL_ARTIFICIAL (decl) = 1;
	  set_decl_location(decl, ad);

	  TYPE_CONTEXT (type) = context;
	  TYPE_NAME (type) = decl;
	  TYPE_STUB_DECL (type) = decl;

	  // Recursively iterator over the anonymous members.
	  fields += layout_aggregate_members(ad->decl, type, inherited_p);

	  // Remove from the anon fields the base offset of this anonymous aggregate.
	  // Undoes what is set-up in setFieldOffset, but doesn't affect accesses.
	  tree offset = size_int(ad->anonoffset);
	  fixup_anonymous_offset(TYPE_FIELDS (type), offset);

	  finish_aggregate_type(ad->anonstructsize, ad->anonalignsize, type, NULL);

	  // And make the corresponding data member.
	  tree field = build_decl(UNKNOWN_LOCATION, FIELD_DECL, NULL, type);
	  insert_aggregate_field(ad->loc, context, field, ad->anonoffset);
	  continue;
	}

      // Other kinds of attributes don't create a scope.
      AttribDeclaration *attrib = sym->isAttribDeclaration();
      if (attrib != NULL)
	{
	  Dsymbols *decl = attrib->include(NULL, NULL);

	  if (decl != NULL)
	    {
	      fields += layout_aggregate_members(decl, context, inherited_p);
	      continue;
	    }
	}

      // Same with template mixins and namespaces.
      if (sym->isTemplateMixin() || sym->isNspace())
	{
	  ScopeDsymbol *scopesym = sym->isScopeDsymbol();
	  if (scopesym->members)
	    {
	      fields += layout_aggregate_members(scopesym->members, context, inherited_p);
	      continue;
	    }
	}
    }

  return fields;
}

// Write out all fields for aggregate BASE.  For classes, write
// out base class fields first, and adds all interfaces last.

void
layout_aggregate_type(AggregateDeclaration *decl, tree type, AggregateDeclaration *base)
{
  ClassDeclaration *cd = base->isClassDeclaration();
  bool inherited_p = (decl != base);

  if (cd != NULL)
    {
      if (cd->baseClass)
	layout_aggregate_type(decl, type, cd->baseClass);
      else
	{
	  // This is the base class (Object) or interface.
	  tree objtype = TREE_TYPE (build_ctype(cd->type));

	  // Add the virtual table pointer, and optionally the monitor fields.
	  tree field = build_decl(UNKNOWN_LOCATION, FIELD_DECL,
				  get_identifier("__vptr"), vtbl_ptr_type_node);
	  DECL_VIRTUAL_P (field) = 1;
	  TYPE_VFIELD (type) = field;
	  DECL_FCONTEXT (field) = objtype;
	  DECL_ARTIFICIAL (field) = 1;
	  DECL_IGNORED_P (field) = inherited_p;
	  insert_aggregate_field(decl->loc, type, field, 0);

	  if (!cd->cpp)
	    {
	      field = build_decl(UNKNOWN_LOCATION, FIELD_DECL,
				 get_identifier("__monitor"), ptr_type_node);
	      DECL_ARTIFICIAL (field) = 1;
	      DECL_IGNORED_P (field) = inherited_p;
	      insert_aggregate_field(decl->loc, type, field, Target::ptrsize);
	    }
	}
    }

  if (base->fields.dim)
    {
      size_t fields = layout_aggregate_members(base->members, type, inherited_p);
      gcc_assert(fields == base->fields.dim);

      // Make sure that all fields have been created.
      if (!inherited_p)
	{
	  for (size_t i = 0; i < base->fields.dim; i++)
	    {
	      VarDeclaration *var = base->fields[i];
	      gcc_assert(var->csym != NULL);
	    }
	}
    }

  if (cd && cd->vtblInterfaces)
    {
      for (size_t i = 0; i < cd->vtblInterfaces->dim; i++)
	{
	  BaseClass *bc = (*cd->vtblInterfaces)[i];
	  tree field = build_decl(UNKNOWN_LOCATION, FIELD_DECL, NULL_TREE,
				  build_ctype(Type::tvoidptr->pointerTo()));
	  DECL_ARTIFICIAL (field) = 1;
	  DECL_IGNORED_P (field) = 1;
	  insert_aggregate_field(decl->loc, type, field, bc->offset);
	}
    }
}

// Add a compiler generated field FIELD at OFFSET into aggregate.

void
insert_aggregate_field(const Loc& loc, tree type, tree field, size_t offset)
{
  DECL_FIELD_CONTEXT (field) = type;
  SET_DECL_OFFSET_ALIGN (field, TYPE_ALIGN (TREE_TYPE (field)));
  DECL_FIELD_OFFSET (field) = size_int(offset);
  DECL_FIELD_BIT_OFFSET (field) = bitsize_zero_node;

  // Must set this or we crash with DWARF debugging.
  set_decl_location(field, loc);

  TREE_THIS_VOLATILE (field) = TYPE_VOLATILE (TREE_TYPE (field));

  layout_decl(field, 0);
  TYPE_FIELDS (type) = chainon(TYPE_FIELDS (type), field);
}

// Create an anonymous field of type ubyte[SIZE] at OFFSET.
// CONTEXT is the record type where the field will be inserted.

static tree
fill_alignment_field(tree context, tree size, tree offset)
{
  tree type = d_array_type(Type::tuns8, tree_to_uhwi(size));
  tree field = build_decl(UNKNOWN_LOCATION, FIELD_DECL, NULL_TREE, type);

  // As per insert_aggregate_field.
  DECL_FIELD_CONTEXT (field) = context;
  SET_DECL_OFFSET_ALIGN (field, TYPE_ALIGN (TREE_TYPE (field)));
  DECL_FIELD_OFFSET (field) = offset;
  DECL_FIELD_BIT_OFFSET (field) = bitsize_zero_node;

  layout_decl(field, 0);

  DECL_ARTIFICIAL (field) = 1;
  DECL_IGNORED_P (field) = 1;

  return field;
}

// Insert anonymous fields in the record TYPE for padding out alignment holes.

static void
fill_alignment_holes(tree type)
{
  // Filling alignment holes this way only applies for structs.
  if (TREE_CODE (type) != RECORD_TYPE
      || CLASS_TYPE_P (type) || TYPE_PACKED (type))
    return;

  tree offset = size_zero_node;
  tree prev = NULL_TREE;

  for (tree field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
    {
      tree voffset = DECL_FIELD_OFFSET (field);
      tree vsize = TYPE_SIZE_UNIT (TREE_TYPE (field));

      // If there is an alignment hole, pad with a static array of type ubyte[].
      if (prev != NULL_TREE && tree_int_cst_lt(offset, voffset))
	{
	  tree psize = size_binop(MINUS_EXPR, voffset, offset);
	  tree pfield = fill_alignment_field(type, psize, offset);

	  // Insert before the current field position.
	  DECL_CHAIN (pfield) = DECL_CHAIN (prev);
	  DECL_CHAIN (prev) = pfield;
	}

      prev = field;
      offset = size_binop(PLUS_EXPR, voffset, vsize);
    }

  // Finally pad out the end of the record.
  if (tree_int_cst_lt(offset, TYPE_SIZE_UNIT (type)))
    {
      tree psize = size_binop(MINUS_EXPR, TYPE_SIZE_UNIT (type), offset);
      tree pfield = fill_alignment_field(type, psize, offset);
      TYPE_FIELDS (type) = chainon(TYPE_FIELDS (type), pfield);
    }
}

// Wrap-up and compute finalised aggregate type.

void
finish_aggregate_type(unsigned structsize, unsigned alignsize, tree type,
		      UserAttributeDeclaration *declattrs)
{
  TYPE_SIZE (type) = NULL_TREE;

  // Write out any GCC attributes that were applied to the type declaration.
  if (declattrs)
    {
      Expressions *attrs = declattrs->getAttributes();
      decl_attributes(&type, build_attributes(attrs),
		      ATTR_FLAG_TYPE_IN_PLACE);
    }

  // Set size and alignment as requested by frontend.
  TYPE_SIZE (type) = bitsize_int(structsize * BITS_PER_UNIT);
  TYPE_SIZE_UNIT (type) = size_int(structsize);
  SET_TYPE_ALIGN (type, alignsize * BITS_PER_UNIT);
  TYPE_PACKED (type) = (alignsize == 1);

  // Add padding to fill in any alignment holes.
  fill_alignment_holes(type);

  // Set the backend type mode.
  compute_record_mode(type);

  // Fix up all variants of this aggregate type.
  for (tree t = TYPE_MAIN_VARIANT (type); t; t = TYPE_NEXT_VARIANT (t))
    {
      if (t == type)
	continue;

      TYPE_FIELDS (t) = TYPE_FIELDS (type);
      TYPE_LANG_SPECIFIC (t) = TYPE_LANG_SPECIFIC (type);
      SET_TYPE_ALIGN (t, TYPE_ALIGN (type));
      TYPE_USER_ALIGN (t) = TYPE_USER_ALIGN (type);
      gcc_assert(TYPE_MODE (t) == TYPE_MODE (type));
    }
}

