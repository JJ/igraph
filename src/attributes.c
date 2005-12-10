/* -*- mode: C -*-  */
/* 
   IGraph library.
   Copyright (C) 2005  Gabor Csardi <csardi@rmki.kfki.hu>
   MTA RMKI, Konkoly-Thege Miklos st. 29-33, Budapest 1121, Hungary
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "attributes.h"
#include "memory.h"
#include "igraph.h"

#include <string.h>

/**
 * \ingroup internal
 */

long int igraph_i_attribute_list_get_pos(igraph_attribute_list_t *al, 
					 const char *name) {
  long int pos=-1;
  long int n=igraph_strvector_size(&al->names);
  bool_t l=0;
  char *str;
  
  while(!l && pos < n-1) {
    igraph_strvector_get(&al->names, pos+1, &str);
    l=!strcmp(name, str);
    pos++;
  }
  
  if (!l) {
    pos = -1;
  }

  return pos;
}

/**
 * \ingroup internal
 */

void igraph_i_attribute_list_free(igraph_attribute_list_t *al, long int i) {
  if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
    vector_t *numv=VECTOR(al->data)[i];
    vector_destroy(numv);
  } else if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t *strv=VECTOR(al->data)[i];
    igraph_strvector_destroy(strv);
  }
}

/**
 * \ingroup internal
 * \brief Initializes an attribute list
 */

int igraph_attribute_list_init(igraph_attribute_list_t *al, long int len) {
  al->len=len;
  IGRAPH_STRVECTOR_INIT_FINALLY(&al->names, 0);
  VECTOR_INIT_FINALLY(&al->types, 0);
  VECTOR_PTR_INIT_FINALLY(&al->data, 0);
  IGRAPH_FINALLY_CLEAN(3);
  return 0;
}

/**
 * \ingroup internal
 * \brief Frees the memory allocated for an attribute list
 */

void igraph_attribute_list_destroy(igraph_attribute_list_t *al) {
  long int i;

  for (i=0; i<vector_size(&al->types); i++) {
    igraph_i_attribute_list_free(al, i);
  }

  igraph_strvector_destroy(&al->names);
  vector_destroy(&al->types);
  vector_ptr_destroy_all(&al->data);
}

/**
 * \ingroup internal
 * \brief Adds a new attribute to an attribute list.
 */

int igraph_attribute_list_add(igraph_attribute_list_t *al,
			      const char *name, igraph_attribute_type_t type){
  long int pos;
  void *data=NULL;

  /* Checks */
  if (strlen(name)==0) {
    IGRAPH_FERROR("invalid attribute name", IGRAPH_EINVAL);
  }
  pos=igraph_i_attribute_list_get_pos(al, name);
  if (pos >= 0) {
    IGRAPH_FERROR("attribute already exists", IGRAPH_EXISTS);
  }

  if (type==IGRAPH_ATTRIBUTE_NUM) {
    data=(void*) Calloc(1, vector_t);
    if (data != 0) { 
      VECTOR_INIT_FINALLY((vector_t*)data, al->len); 
    } else {
      IGRAPH_FERROR("cannot add attribute", IGRAPH_ENOMEM);
    }
  } else /* if (type==IGRAPH_ATTRIBUTE_STR) */ {
    data=(void*)Calloc(1, igraph_strvector_t);
    if (data != 0) { 
      IGRAPH_STRVECTOR_INIT_FINALLY((igraph_strvector_t*)data, al->len);
    } else {
      IGRAPH_FERROR("cannot add attribute", IGRAPH_ENOMEM);
    }
  }

  IGRAPH_CHECK(vector_ptr_reserve(&al->data, vector_ptr_size(&al->data)+1));
  IGRAPH_CHECK(vector_reserve(&al->types, vector_size(&al->types)+1));
  IGRAPH_CHECK(igraph_strvector_add(&al->names, name));
    
  vector_ptr_push_back(&al->data, data);
  vector_push_back(&al->types, type);
  /* Space is allocated already, no need to check errors... */

  IGRAPH_FINALLY_CLEAN(1);
  return 0;
}

/**
 * \ingroup internal
 * \brief Removes an attribute from an attribute list.
 */

int igraph_attribute_list_remove(igraph_attribute_list_t *al, 
				 const char *name) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  void* ptr;
  
  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  igraph_i_attribute_list_free(al, pos);
  ptr=vector_ptr_e(&al->data, pos);
  Free(ptr);
  vector_ptr_remove(&al->data, pos);
  vector_remove(&al->types, pos);
  igraph_strvector_remove(&al->names, pos);
  return 0;
}

/**
 * \ingroup internal
 * \brief Returns an attribute for a single element 
 */

int igraph_attribute_list_get(igraph_attribute_list_t *al, const char *name, 
			      long int idx, void **value, 
			      igraph_attribute_type_t *type) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  igraph_attribute_type_t atype;

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  atype=VECTOR(al->types)[pos];
  if (type != 0) {
    *type = atype;
  }
  if (atype==IGRAPH_ATTRIBUTE_NUM) {
    vector_t *data=VECTOR(al->data)[pos];
    *value=(void*) vector_e_ptr(data, idx);
  } else if (atype==IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t *data=VECTOR(al->data)[pos];
    igraph_strvector_get(data, idx, (char**)value);
  }
  return 0;
}

/**
 * \ingroup internal
 * \brief Sets an attribute for a single element
 */

int igraph_attribute_list_set(igraph_attribute_list_t *al, const char *name,
			      long int idx, void *value) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  igraph_attribute_type_t atype;

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  atype=VECTOR(al->types)[pos];
  if (atype==IGRAPH_ATTRIBUTE_NUM) {
    vector_t *data=VECTOR(al->data)[pos];
    vector_set(data, idx, *(real_t*)(value));
  } else if (atype==IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t *data=VECTOR(al->data)[pos];
    IGRAPH_CHECK(igraph_strvector_set(data, idx, (char*)value));
  }
  return 0;
}

/**
 * \ingroup internal
 * \brief Returns an attribute for many elements
 */

int igraph_attribute_list_get_many(igraph_attribute_list_t *al, 
				   const char *name,
				   vector_t *idx, void **value) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  igraph_attribute_type_t atype;
  long int i;

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  atype=VECTOR(al->types)[pos];
  if (atype==IGRAPH_ATTRIBUTE_NUM) {
    vector_t *data=VECTOR(al->data)[pos];
    vector_t *nvalue=*value;
    IGRAPH_CHECK(vector_resize(nvalue, vector_size(idx)));
    for (i=0; i<vector_size(idx); i++) {
      VECTOR(*nvalue)[i] = VECTOR(*data)[ (long int)VECTOR(*idx)[i] ];
    }
  } else if (atype==IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t *data=VECTOR(al->data)[pos];
    igraph_strvector_t *svalue=*value;
    IGRAPH_CHECK(igraph_strvector_resize(svalue, vector_size(idx)));
    for (i=0; i<vector_size(idx); i++) {
      char *str;
      igraph_strvector_get(data, VECTOR(*idx)[i], &str);
      IGRAPH_CHECK(igraph_strvector_set(svalue, i, str));
    }    
  }
  return 0;
}

/**
 * \ingroup internal
 * \brief Sets an attribute for many elements
 */

int igraph_attribute_list_set_many(igraph_attribute_list_t *al, 
				   const char *name,
				   vector_t *idx, void *value) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  igraph_attribute_type_t atype;
  long int i;

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  atype=VECTOR(al->types)[pos];
  if (atype==IGRAPH_ATTRIBUTE_NUM) {
    vector_t *data=VECTOR(al->data)[pos];
    vector_t *nvalue=value;
    long int idxlen=vector_size(nvalue);
    long int j=0;
    for (i=0; i<vector_size(idx); i++) {
      VECTOR(*data)[ (long int)VECTOR(*idx)[i] ] = VECTOR(*nvalue)[j++];
      if (j>=idxlen) { j=0; }
    }
  } else if (atype==IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t bak;
    igraph_strvector_t *data=VECTOR(al->data)[pos];
    igraph_strvector_t *svalue=value;
    long int idxlen=igraph_strvector_size(svalue);
    long int j=0;
    IGRAPH_CHECK(igraph_strvector_copy(&bak, data));
    IGRAPH_FINALLY(igraph_strvector_destroy, &bak);
    for (i=0; i<vector_size(idx); i++) {
      char *str;
      igraph_strvector_get(svalue, j++, &str);
      IGRAPH_CHECK(igraph_strvector_set(&bak, VECTOR(*idx)[i], str));
      if (j>=idxlen) { j=0; }
    }
    IGRAPH_FINALLY_CLEAN(1);
    igraph_strvector_destroy(data);
    *data=bak;
  }

  return 0;
}

/**
 * \ingroup internal
 * \brief Returns an attribute for all elements (untested!)
 */

int igraph_attribute_list_get_all(igraph_attribute_list_t *al, 
				  const char *name, void **value, 
				  igraph_attribute_type_t *type) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  igraph_attribute_type_t atype=IGRAPH_ATTRIBUTE_NUM;

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }
  
  if (type != 0) {
    *type=atype;
  }
  
  atype=VECTOR(al->types)[pos];
  if (atype==IGRAPH_ATTRIBUTE_NUM) {
    vector_t *data=VECTOR(al->data)[pos];
    vector_t *nvalue=*value;
    vector_t tmp=*nvalue;
    IGRAPH_CHECK(vector_copy(nvalue, data));
    vector_destroy(&tmp);
  } else if (atype==IGRAPH_ATTRIBUTE_STR) {
    igraph_strvector_t *data=VECTOR(al->data)[pos];
    igraph_strvector_t *svalue=*value;
    igraph_strvector_t tmp=*svalue;
    IGRAPH_CHECK(igraph_strvector_copy(svalue, data));
    igraph_strvector_destroy(&tmp);
  }

  return 0;
}

/**
 * \ingroup internal
 * \brief Returns the number of attributes in an attribute list
 */

long int igraph_attribute_list_size(igraph_attribute_list_t *al) {
  return vector_size(&al->types);
}

/**
 * \ingroup internal
 * \brief Adds new elements to an attribute list (not attributes, elements!)
 */

int igraph_attribute_list_add_elem(igraph_attribute_list_t *al, long int ne) {
  long int i;
  int ret;
  bool_t error=0;
  igraph_error_handler_t *oldhandler;
  
  oldhandler=igraph_set_error_handler(igraph_error_handler_ignore);
  for (i=0; i<vector_size(&al->types); i++) {
    if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
      vector_t *data=VECTOR(al->data)[i];
      ret=vector_resize(data, al->len+ne);
      if (ret != 0) {
	error=1; break;
      }
    } else if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_STR) {
      igraph_strvector_t *data=VECTOR(al->data)[i];
      ret=igraph_strvector_resize(data, al->len+ne);
      if (ret != 0) {
	error=1; break;
      }
    }
  }
  al->len += ne;

  if (error) {
    al->len -= ne;
    for (i=0; i<vector_size(&al->types); i++) {
      if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
	vector_t *data=VECTOR(al->data)[i];
	vector_resize(data, al->len);
      } else if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_STR) {
	igraph_strvector_t *data=VECTOR(al->data)[i];
	igraph_strvector_resize(data, al->len);
      }
    }
  }

  igraph_set_error_handler(oldhandler);
  return 0;
}

/**
 * \ingroup internal
 * \brief Returns names of the attributes in an attribute list
 */

int igraph_attribute_list_names(igraph_attribute_list_t *al,
				igraph_strvector_t *names, vector_t *types) {
  if (names != 0) {
    igraph_strvector_t tmp=*names;
    IGRAPH_CHECK(igraph_strvector_copy(names, &al->names));
    igraph_strvector_destroy(&tmp);
  }
  if (types != 0) {
    vector_t tmp=*types;
    IGRAPH_CHECK(vector_copy(types, &al->types));
    vector_destroy(&tmp);
  }
  
  return 0;
}

/**
 * \ingroup internal
 * \brief Creates a (deep) copy of an attribute list
 */

int igraph_attribute_list_copy(igraph_attribute_list_t *to,
			       igraph_attribute_list_t *from) {
  long int i;
  igraph_error_handler_t *oldhandler;
  bool_t error=0;

  to->len=from->len;
  IGRAPH_CHECK(igraph_strvector_copy(&to->names, &from->names));
  IGRAPH_FINALLY(&to->names, igraph_strvector_destroy);  
  IGRAPH_CHECK(vector_copy(&to->types, &from->types));
  IGRAPH_FINALLY(&to->types, vector_destroy);
  IGRAPH_CHECK(vector_ptr_copy(&to->data, &from->data));
  vector_ptr_null(&to->data);

  oldhandler=igraph_set_error_handler(igraph_error_handler_ignore);
  for (i=0; i<vector_size(&from->types); i++) {
    int ret;
    if (VECTOR(from->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
      vector_t *data=VECTOR(from->data)[i];
      vector_t *ndata=Calloc(1, vector_t);
      if (ndata==0) {
	error=1; break;
      }
      ret=vector_copy(ndata, data);
      if (ret != 0) {
	error=1; break;
      }
      VECTOR(to->data)[i]=ndata;
    } else if (VECTOR(from->types)[i] == IGRAPH_ATTRIBUTE_STR) {
      igraph_strvector_t *data=VECTOR(from->data)[i];
      igraph_strvector_t *ndata=Calloc(1, igraph_strvector_t);
      if (ndata==0) {
	error=1; break;
      }
      ret=igraph_strvector_copy(ndata, data);
      if (ret != 0) {
	error=1; break;
      }
      VECTOR(to->data)[i]=ndata;
    }
  }
  
  if (error != 0) {
    /* names & types are deleted already */
    for (i=0; i<vector_size(&from->types); i++) {
      if (VECTOR(from->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
	vector_t *data=VECTOR(to->data)[i];
	if (data != 0) {
	  vector_destroy(data);
	  Free(data);
	}
      } else if (VECTOR(from->types)[i] == IGRAPH_ATTRIBUTE_STR) {
	igraph_strvector_t *data=VECTOR(to->data)[i];
	if (data != 0) {
	  igraph_strvector_destroy(data);
	  Free(data);
	}
      }
    }
    vector_ptr_destroy(&to->data);
  }
  
  igraph_set_error_handler(oldhandler);

  if (!error) { IGRAPH_FINALLY_CLEAN(2); }
  return 0;
}

/**
 * \ingroup internal
 * \brief Returns 0 if the given attribute exists and returns
 * its type in the third argument. Returns an error code otherwise.
 */

int igraph_attribute_list_get_type(igraph_attribute_list_t *al, 
				   const char *name,
				   igraph_attribute_type_t *type) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);

  if (pos < 0) {
    IGRAPH_FERROR("no such attribute", IGRAPH_EINVAL);
  }

  if (type) *type=VECTOR(al->types)[pos];
  return 0;
}

/**
 * \ingroup internal
 * \brief Removes elements (not attributes!) from an attribute list
 * (in a weird way)
 */

void igraph_attribute_list_remove_elem_idx(igraph_attribute_list_t *al, 
					  long int *index, long int nremove) {
  long int i;
  al->len -= nremove;
  
  for (i=0; i<vector_size(&al->types); i++) {
    if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
      vector_t *data=VECTOR(al->data)[i];
      vector_permdelete(data, index, nremove);
    } else if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_STR) {
      igraph_strvector_t *data=VECTOR(al->data)[i];
      igraph_strvector_permdelete(data, index, nremove);
    }
  }
}

/**
 * \ingroup internal
 * \brief Removes elements (not attributes!) from an attribute list
 * (in another weird way)
 */

void igraph_attribute_list_remove_elem_neg(igraph_attribute_list_t *al,
					  vector_t *neg, long int nremove) {
  long int i;
  al->len -= nremove;
  
  for (i=0; i<vector_size(&al->types); i++) {
    if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_NUM) {
      vector_t *data=VECTOR(al->data)[i];
      vector_remove_negidx(data, neg, nremove);
    } else if (VECTOR(al->types)[i] == IGRAPH_ATTRIBUTE_STR) {
      igraph_strvector_t *data=VECTOR(al->data)[i];
      igraph_strvector_remove_negidx(data, neg, nremove);
    }
  }
}

/**
 * \ingroup internal
 * \brief Checks whether the list contains the named attribute
 */

bool_t igraph_attribute_list_has(igraph_attribute_list_t *al, 
				 const char *name) {
  long int pos=igraph_i_attribute_list_get_pos(al, name);
  return (pos != -1);
}

/* ------------------------------------------------------------------------- */

/**
 * \ingroup attributes
 * \brief Adds a graph attribute.
 *
 * Attributes have to be added by calling this function before setting
 * or getting them.
 * @param graph A graph object.
 * @param name The name of the attribute to install.
 * @param type Numeric constant giving the type of the attribute,
 *        either <b>IGRAPH_ATTRIBUTE_NUM</b> (numeric) or
 *        <b>IGRAPH_ATTRIBUTE_STR</b> (string).
 * @return Error code:
 *         - <B>IGRAPH_EINVAL</B>: invalid (empty) attribute name
 *         - <B>IGRAPH_EXISTS</B>: the attribute already exists
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of graph
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_add_graph_attribute(igraph_t *graph, const char *name,
			       igraph_attribute_type_t type) {
  return igraph_attribute_list_add(&graph->gal, name, type);
}

/**
 * \ingroup attributes
 * \brief Removes a graph attribute.
 *
 * @param graph A graph object.
 * @param name The name of the attribute to remove.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of graph
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_remove_graph_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_remove(&graph->gal, name);
}

/**
 * \ingroup attributes
 * \brief Queries the value of a graph attribute.
 *
 * @param graph A graph object.
 * @param name The name of the attribute to query.
 * @param value Pointer to a typeless pointer. The address of the
 *        result will be stored here, a <code>real_t</code> pointer
 *        for numeric attributes or a <code>const char</code> pointer
 *        to string attributes.
 * @param type Pointer to the attribute type, it will be stored here
 *        if not <code>NULL</code>.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of graph
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_get_graph_attribute(igraph_t *graph, const char *name,
			       void **value, igraph_attribute_type_t *type) {
  return igraph_attribute_list_get(&graph->gal, name, 0, value, type);
}

/**
 * \ingroup attributes
 * \brief Sets the value of a graph attribute.
 *
 * @param graph A graph object.
 * @param name The name of the attribute to set.
 * @param value Pointer to the new value of the attribute, either a
 *        <code>real_t</code> or a <code>const char</code> pointer.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of graph
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_set_graph_attribute(igraph_t *graph, const char *name,
			       void *value) {
  return igraph_attribute_list_set(&graph->gal, name, 0, value);
}

/**
 * \ingroup attributes
 * \brief Queries the list of installed graph attributes.
 *
 * @param graph A graph object.
 * @param names This string vector will contain the names of the
 *        attributes. It should be initialized and will be resized.
 *        This parameter can be <code>NULL</code>, which means that it
 *        is ignored.
 * @param types Pointer to a vector, this will be set to the types of
 *        the attributes if the pointer is not <code>NULL</code>. The
 *        vector will be resized if neccessary.
 * @return Error code.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of graph
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_list_graph_attributes(igraph_t *graph, igraph_strvector_t *names,
				 vector_t *types) {
  return igraph_attribute_list_names(&graph->gal, names, types);
}

/**
 * \ingroup attributes
 * \brief Adds a vertex attribute.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to install.
 * @param type Numeric constant giving the type of the attribute,
 *        either <b>IGRAPH_ATTRIBUTE_NUM</b> (numeric) or
 *        <b>IGRAPH_ATTRIBUTE_STR</b> (string).
 * @return Error code:
 *         - <B>IGRAPH_EINVAL</B>: invalid (empty) attribute name
 *         - <B>IGRAPH_EXISTS</B>: the attribute already exists
 *
 * Time complexity: <code>O(|V|)</code>, the number of vertices in the
 * graph.
 */

int igraph_add_vertex_attribute(igraph_t *graph, const char *name,
				igraph_attribute_type_t type) {
  return igraph_attribute_list_add(&graph->val, name, type);
}

/**
 * \ingroup attributes
 * \brief Removes a vertex attribute.
 *
 * @param graph A graph object.
 * @param name The name of the attribute to remove.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|V|)</code>, assuming that the graph has
 * <code>O(1)</code> vertex attributes. <code>|V|</code> is the number
 * of vertices.
 */

int igraph_remove_vertex_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_remove(&graph->val, name);
}

/**
 * \ingroup attributes
 * \brief Queries the value of a vertex attribute for a single vertex
 *
 * @param graph The graph object.
 * @param name The name of the vertex attribute.
 * @param v The id of the vertex of which the attribute is requested.
 * @param value Pointer to a typeless pointer. The address of the
 *        result will be stored here, a <code>real_t</code> pointer
 *        for numeric attributes or a <code>const char</code> pointer
 *        to string attributes.
 * @param type If not <code>NULL</code> the type of the attribute will
 *        be stored here.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>, assuming that the graph has
 * <code>O(1)</code> vertex attributes installed.
 */

int igraph_get_vertex_attribute(igraph_t *graph, const char *name,
				long int v, void **value,
				igraph_attribute_type_t *type) {
  return igraph_attribute_list_get(&graph->val, name, v, value, type);
}

/**
 * \ingroup attributes
 * \brief Set the value of a vertex attribute for a single vertex.
 *
 * @param graph The graph object.
 * @param name Name of the vertex attribute.
 * @param v The id of the vertex of which the attribute is set.
 * @param value The new value of the attribute.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>, assuming that the graph has
 * <code>O(1)</code> vertex attributes installed.
 */

int igraph_set_vertex_attribute(igraph_t *graph, const char *name,
				long int v, void* value) {
  return igraph_attribute_list_set(&graph->val, name, v, value);
}

/**
 * \ingroup attributes
 * \brief Query the value of a vertex attribute for many vertices.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to get.
 * @param v Vector with the vertex ids of the vertices of which the
 *        attribute will be returned.
 * @param value Pointer to a typeless pointer, which contains the
 *        address of either a <code>vector_t</code> or a
 *        <code>igraph_strvector_t</code> depending on the type of the
 *        attribute.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|v|)</code>, the number of queried
 * vertices, assuming the graph has <code>O(1)</code> vertex
 * attributes.
 */

int igraph_get_vertex_attributes(igraph_t *graph, const char *name,
				 igraph_vs_t v, void **value) {
  int ret;
  igraph_vs_t myv;
  IGRAPH_CHECK(igraph_vs_create_view_as_vector(graph, &v, &myv));
  IGRAPH_FINALLY(igraph_vs_destroy, &myv);
  ret=igraph_attribute_list_get_many(&graph->val, name, myv.v, value);
  igraph_vs_destroy(&myv);
  IGRAPH_FINALLY_CLEAN(1);
  return ret;
}

/**
 * \ingroup attributes
 * \brief Set the value of a vertex attribute for many vertices.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to set.
 * @param v Vector with the vertex ids of the vertices of which the
 *        attribute will be set.
 * @param value The new value(s) of the attribute. This may be of
 *        different length than <code>v</code>, if it is shorter it
 *        will be recycled (ie. after the last element the first one
 *        is used again), if it is longer the unneeded values are
 *        ignored. Thus it is easy to set an attribute to a single
 *        constant value for many vertices, just give a vector of
 *        length 1 here. This parameter is either a pointer to a
 *        <code>vector_t</code> or an <code>igraph_strvector_t</code>, 
 *        depending on the type of the attribute.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|v|)</code>, the number of affected
 * vertices, assuming the graph has <code>O(1)</code> vertex
 * attributes.
 */

int igraph_set_vertex_attributes(igraph_t *graph, const char *name,
				 igraph_vs_t v, void *value) {
  int ret;
  igraph_vs_t myv;
  IGRAPH_CHECK(igraph_vs_create_view_as_vector(graph, &v, &myv));
  IGRAPH_FINALLY(igraph_vs_destroy, &myv);
  ret=igraph_attribute_list_set_many(&graph->val, name, myv.v, value);
  igraph_vs_destroy(&myv);
  IGRAPH_FINALLY_CLEAN(1);
  return ret;
}

/**
 * \ingroup attributes
 * \brief Queries the list of installed vertex attributes.
 *
 * @param graph A graph object.
 * @param l This string array will contain the names of the
 *        attributes. It should be initialized and will be resized.
 *        If <code>NULL</code> then this parameter is ignored.
 * @param types Pointer to a vector, this will be set to the types of
 *        the attributes if the pointer is not <code>NULL</code>. The
 *        vector will be resized if neccessary.
 * @return Error code.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of vertex
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_list_vertex_attributes(igraph_t *graph, igraph_strvector_t *l,
				  vector_t *types) {
  return igraph_attribute_list_names(&graph->val, l, types);
}

/**
 * \ingroup attributes
 * \brief Adds an edge attribute.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to install.
 * @param type Numeric constant giving the type of the attribute,
 *        either <b>IGRAPH_ATTRIBUTE_NUM</b> (numeric) or
 *        <b>IGRAPH_ATTRIBUTE_STR</b> (string).
 * @return Error code:
 *         - <B>IGRAPH_EINVAL</B>: invalid (empty) attribute name
 *         - <B>IGRAPH_EXISTS</B>: the attribute already exists
 *
 * Time complexity: <code>O(|E|)</code>, the number of edges in the
 * graph.
 */

int igraph_add_edge_attribute(igraph_t *graph, const char *name,
			      igraph_attribute_type_t type) {
  return igraph_attribute_list_add(&graph->eal, name, type);
}

/**
 * \ingroup attributes
 * \brief Removes an edge attribute.
 *
 * @param graph A graph object.
 * @param name The name of the attribute to remove.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|E|)</code>, assuming that the graph has
 * <code>O(1)</code> edge attributes. <code>|E|</code> is the number
 * of edges.
 */

int igraph_remove_edge_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_remove(&graph->eal, name);
}

/**
 * \ingroup attributes
 * \brief Queries the value of an edge attribute for a single edge
 *
 * It is easy to get the id of an edge by using an edge iterator.
 * @param graph The graph object.
 * @param name The name of the edge attribute.
 * @param e The id of the edge of which the attribute is requested.
 * @param value Pointer to a typeless pointer. The address of the
 *        result will be placed here, a <code>real_t</code> pointer
 *        for numeric attributes and a <code>const char</code> pointer
 *        for string attributes. 
 * @param type If not <code>NULL</code> then the type of the attribute
 *        will be stored here.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>, assuming that the graph has
 * <code>O(1)</code> edge attributes installed.
 */

int igraph_get_edge_attribute(igraph_t *graph, const char *name,
			      long int e, void **value,
			      igraph_attribute_type_t *type) {
  return igraph_attribute_list_get(&graph->eal, name, e, value, type);
}

/**
 * \ingroup attributes
 * \brief Set the value of an edge attribute for a single edge.
 *
 * @param graph The graph object.
 * @param name Name of the edge attribute.
 * @param e The id of the edge of which the attribute is set.
 * @param value The new value of the attribute. Pointer to a
 *        <code>real_t</code> or a <code>const char</code>.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code>, assuming that the graph has
 * <code>O(1)</code> edge attributes installed.
 */

int igraph_set_edge_attribute(igraph_t *graph, const char *name,
			      long int e, void *value) {
  return igraph_attribute_list_set(&graph->eal, name, e, value);
}

/**
 * \ingroup attributes
 * \brief Query the value of an edge attribute for many edges.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to get.
 * @param e Vector with the edge ids of the edges of which the
 *        attribute will be returned.
 * @param value Pointer to a typeless pointer, which contains the
 *        address of either a <code>vector_t</code> or a
 *        <code>igraph_strvector_t</code> depending on the type of the
 *        attribute.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|e|)</code>, the number of queried
 * edges, assuming the graph has <code>O(1)</code> edge
 * attributes.
 */

int igraph_get_edge_attributes(igraph_t *graph, const char *name,
			       vector_t *e, void **value) {
  return igraph_attribute_list_get_many(&graph->eal, name, e, value);
}

/**
 * \ingroup attributes
 * \brief Set the value of an edge attribute for many edges.
 *
 * @param graph The graph object.
 * @param name The name of the attribute to set.
 * @param e Vector with the edge ids of the edges of which the
 *        attribute will be set.
 * @param value The new value(s) of the attribute. This may be of
 *        different length than <code>v</code>, if it is shorter it
 *        will be recycled (ie. after the last element the first one
 *        is used again), if it is longer the unneeded values are
 *        ignored. Thus it is easy to set an attribute to a single
 *        constant value for many vertices, just give a vector of
 *        length 1 here. This parameter is either a pointer to a
 *        <code>vector_t</code> or an <code>igraph_strvector_t</code>, 
 *        depending on the type of the attribute.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(|v|)</code>, the number of affected
 * edges, assuming the graph has <code>O(1)</code> edge
 * attributes.
 */

int igraph_set_edge_attributes(igraph_t *graph, const char *name,
			       vector_t *e, void *value) {
  return igraph_attribute_list_set_many(&graph->eal, name, e, value);
}

/**
 * \ingroup attributes
 * \brief Queries the list of installed edge attributes.
 *
 * @param graph A graph object.
 * @param l This string array will contain the names of the
 *        attributes (if not <code>NULL</code>). It should be
 *        initialized and will be resized.
 * @param types Pointer to a vector, this will be set to the types of
 *        the attributes if the pointer is not <code>NULL</code>. The
 *        vector will be resized if neccessary.
 * @return Error code.
 *
 * Time complexity: <code>O(1)</code>. (Assuming the number of edge
 * attributes of <code>graph</code> is <code>O(1)</code>.)
 */

int igraph_list_edge_attributes(igraph_t *graph, igraph_strvector_t *l,
				vector_t *types) {
  return igraph_attribute_list_names(&graph->eal, l, types);
}

/**
 * \ingroup attributes
 * \brief Queries the type of a graph attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the attribute.
 * @param type Pointer to the attribute type.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> graph attributes.
 */

int igraph_get_graph_attribute_type(igraph_t *graph, const char *name,
				    igraph_attribute_type_t *type) {
  return igraph_attribute_list_get_type(&graph->gal, name, type);
}

/**
 * \ingroup attributes
 * \brief Queries the type of a vertex attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the attribute.
 * @param type Pointer to the attribute type.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> vertex attributes.
 */

int igraph_get_vertex_attribute_type(igraph_t *graph, const char *name,
				     igraph_attribute_type_t *type) {
  return igraph_attribute_list_get_type(&graph->val, name, type);
}

/**
 * \ingroup attributes
 * \brief Queries the type of an edge attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the attribute.
 * @param type Pointer to the attribute type.
 * @return Error code:
 *         - <b>IGRAPH_EINVAL</b>: the attribute does not exist.
 *
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> edge attributes.
 */

int igraph_get_edge_attribute_type(igraph_t *graph, const char *name,
				   igraph_attribute_type_t *type) {
  return igraph_attribute_list_get_type(&graph->eal, name, type);
}

/**
 * \ingroup attributes
 * \brief Checks whether a graph has the named graph attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the (potential) attribute.
 * @return Non-zero (TRUE) value if the attribute is installed, zero
 *         (FALSE) otherwise.
 * 
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> edge attributes.
 */

bool_t igraph_has_graph_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_has(&graph->gal, name); 
}

/**
 * \ingroup attributes
 * \brief Checks whether a graph has the named vertex attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the (potential) attribute.
 * @return Non-zero (TRUE) value if the attribute is installed, zero
 *         (FALSE) otherwise.
 * 
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> edge attributes.
 */

bool_t igraph_has_vertex_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_has(&graph->val, name); 
}

/**
 * \ingroup attributes
 * \brief Checks whether a graph has the named edge attribute.
 * 
 * @param graph The graph object.
 * @param name The name of the (potential) attribute.
 * @return Non-zero (TRUE) value if the attribute is installed, zero
 *         (FALSE) otherwise.
 * 
 * Time complexity: <code>O(1)</code> assuming there are
 * <code>O(1)</code> edge attributes.
 */

bool_t igraph_has_edge_attribute(igraph_t *graph, const char *name) {
  return igraph_attribute_list_has(&graph->eal, name); 
}
