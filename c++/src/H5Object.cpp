/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <string>

#include "H5private.h"		// for HDmemset
#include "H5Include.h"
#include "H5Exception.h"
#include "H5IdComponent.h"
#include "H5PropList.h"
#include "H5FaccProp.h"
#include "H5FcreatProp.h"
#include "H5OcreatProp.h"
#include "H5DcreatProp.h"
#include "H5DxferProp.h"
#include "H5Location.h"
#include "H5Object.h"
#include "H5DataType.h"
#include "H5DataSpace.h"
#include "H5AbstractDs.h"
#include "H5CommonFG.h"
#include "H5Group.h"
#include "H5File.h"
#include "H5DataSet.h"
#include "H5Attribute.h"

namespace H5 {

#ifndef DOXYGEN_SHOULD_SKIP_THIS
// userAttrOpWrpr simply interfaces between the user's function and the
// C library function H5Aiterate2; used to resolve the different prototype
// problem.  May be moved to Iterator later.
extern "C" herr_t userAttrOpWrpr(hid_t loc_id, const char *attr_name,
    const H5A_info_t *ainfo, void *op_data)
{
   H5std_string s_attr_name = H5std_string( attr_name );
   UserData4Aiterate* myData = reinterpret_cast<UserData4Aiterate *> (op_data);
   myData->op( *myData->location, s_attr_name, myData->opData );
   return 0;
}
#endif

//--------------------------------------------------------------------------
// Function:	H5Object default constructor (protected)
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
H5Object::H5Object() : H5Location() {}

//--------------------------------------------------------------------------
// Function:	H5Object overloaded constructor (protected)
// Purpose	Creates an H5Object object using the id of an existing HDF5
// 		object.
// Parameters	object_id - IN: Id of an existing HDF5 object
// Programmer	Binh-Minh Ribler - 2000
// *** Deprecation warning ***
// This constructor is no longer appropriate because the data member "id" had
// been moved to the sub-classes.  It will be removed in 1.10 release.  If its
// removal does not raise any problems in 1.10, it will be removed from 1.8 in
// subsequent releases.
// Removed in 1.10.1 - Aug 2016
//--------------------------------------------------------------------------
//H5Object::H5Object(const hid_t object_id) : H5Location() {}

//--------------------------------------------------------------------------
// Function:	H5Object copy constructor
///\brief	Copy constructor: makes a copy of the original H5Object
///		instance.
///\param	original - IN: H5Object instance to copy
// Programmer	Binh-Minh Ribler - 2000
// *** Deprecation warning ***
// This constructor is no longer appropriate because the data member "id" had
// been moved to the sub-classes.  It is removed from 1.8.15 because it is
// a noop and it can be generated by the compiler if needed.
//--------------------------------------------------------------------------
// H5Object::H5Object(const H5Object& original) : H5Location() {}

//--------------------------------------------------------------------------
// Function:    f_Attribute_setId - friend
// Purpose:     This function is friend to class H5::Attribute so that it
//              can set Attribute::id in order to work around a problem
//              described in the JIRA issue HDFFV-7947.
//              Applications shouldn't need to use it.
// param        attr   - IN/OUT: Attribute object to be changed
// param        new_id - IN: New id to set
// Programmer   Binh-Minh Ribler - 2015
//--------------------------------------------------------------------------
void f_Attribute_setId(Attribute* attr, hid_t new_id)
{
    attr->p_setId(new_id);
}

//--------------------------------------------------------------------------
// Function:	H5Object::createAttribute
///\brief	Creates an attribute for a group, dataset, or named datatype.
///\param	name - IN: Name of the attribute
///\param	data_type - IN: Datatype for the attribute
///\param	data_space - IN: Dataspace for the attribute - only simple
///		dataspaces are allowed at this time
///\param	create_plist - IN: Creation property list - default to
///		PropList::DEFAULT
///\return	Attribute instance
///\exception	H5::AttributeIException
///\par Description
///		The attribute name specified in \a name must be unique.
///		Attempting to create an attribute with the same name as an
///		existing attribute will raise an exception, leaving the
///		pre-existing attribute intact. To overwrite an existing
///		attribute with a new attribute of the same name, first
///		delete the existing one with \c H5Object::removeAttr, then
///		recreate it with this function.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
Attribute H5Object::createAttribute( const char* name, const DataType& data_type, const DataSpace& data_space, const PropList& create_plist ) const
{
   hid_t type_id = data_type.getId();
   hid_t space_id = data_space.getId();
   hid_t plist_id = create_plist.getId();
   hid_t attr_id = H5Acreate2(getId(), name, type_id, space_id, plist_id, H5P_DEFAULT );

   // If the attribute id is valid, create and return the Attribute object
   if( attr_id > 0 )
   {
	Attribute attr;
	f_Attribute_setId(&attr, attr_id);
	return( attr );
   }
   else
      throw AttributeIException(inMemFunc("createAttribute"), "H5Acreate2 failed");
}

//--------------------------------------------------------------------------
// Function:	H5Object::createAttribute
///\brief	This is an overloaded member function, provided for convenience.
///		It differs from the above function in that it takes
///		a reference to an \c H5std_string for \a name.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
Attribute H5Object::createAttribute( const H5std_string& name, const DataType& data_type, const DataSpace& data_space, const PropList& create_plist ) const
{
   return( createAttribute( name.c_str(), data_type, data_space, create_plist ));
}

//--------------------------------------------------------------------------
// Function:	H5Object::openAttribute
///\brief	Opens an attribute given its name.
///\param	name - IN: Name of the attribute
///\return	Attribute instance
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
Attribute H5Object::openAttribute( const char* name ) const
{
   hid_t attr_id = H5Aopen(getId(), name, H5P_DEFAULT);
   if( attr_id > 0 )
   {
	Attribute attr;
	f_Attribute_setId(&attr, attr_id);
	return( attr );
   }
   else
   {
      throw AttributeIException(inMemFunc("openAttribute"), "H5Aopen failed");
   }
}

//--------------------------------------------------------------------------
// Function:	H5Object::openAttribute
///\brief	This is an overloaded member function, provided for convenience.
///		It differs from the above function in that it takes
///		a reference to an \c H5std_string for \a name.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
Attribute H5Object::openAttribute( const H5std_string& name ) const
{
   return( openAttribute( name.c_str()) );
}

//--------------------------------------------------------------------------
// Function:	H5Object::openAttribute
///\brief	Opens an attribute given its index.
///\param	idx - IN: Index of the attribute, a 0-based, non-negative integer
///\return	Attribute instance
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
Attribute H5Object::openAttribute( const unsigned int idx ) const
{
   hid_t attr_id = H5Aopen_by_idx(getId(), ".", H5_INDEX_CRT_ORDER,
		H5_ITER_INC, static_cast<hsize_t>(idx), H5P_DEFAULT, H5P_DEFAULT);
   if( attr_id > 0 )
   {
	Attribute attr;
	f_Attribute_setId(&attr, attr_id);
	return(attr);
   }
   else
   {
	throw AttributeIException(inMemFunc("openAttribute"), "H5Aopen_by_idx failed");
   }
}

//--------------------------------------------------------------------------
// Function:	H5Object::iterateAttrs
///\brief	Iterates a user's function over all the attributes of an H5
///		object, which may be a group, dataset or named datatype.
///\param	user_op - IN: User's function to operate on each attribute
///\param	_idx - IN/OUT: Starting (IN) and ending (OUT) attribute indices
///\param	op_data - IN: User's data to pass to user's operator function
///\return	Returned value of the last operator if it was non-zero, or
///		zero if all attributes were processed
///\exception	H5::AttributeIException
///\par Description
///		The signature of user_op is
///		void (*)(H5::H5Location&, H5std_string, void*).
///		For information, please refer to the C layer Reference Manual
///		at:
/// http://www.hdfgroup.org/HDF5/doc/RM/RM_H5A.html#Annot-Iterate
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
int H5Object::iterateAttrs( attr_operator_t user_op, unsigned *_idx, void *op_data )
{
   // store the user's function and data
   UserData4Aiterate* userData = new UserData4Aiterate;
   userData->opData = op_data;
   userData->op = user_op;
   userData->location = this;

   // call the C library routine H5Aiterate2 to iterate the attributes
   hsize_t idx = _idx ? static_cast<hsize_t>(*_idx) : 0;
   int ret_value = H5Aiterate2(getId(), H5_INDEX_NAME, H5_ITER_INC, &idx,
			userAttrOpWrpr, reinterpret_cast<void *>(userData));

   // release memory
   delete userData;

   if( ret_value >= 0 ) {
      /* Pass back update index value to calling code */
      if (_idx)
	 *_idx = static_cast<unsigned>(idx);

      return( ret_value );
   }
   else  // raise exception when H5Aiterate returns a negative value
      throw AttributeIException(inMemFunc("iterateAttrs"), "H5Aiterate2 failed");
}

//--------------------------------------------------------------------------
// Function:	H5Object::getNumAttrs
///\brief	Returns the number of attributes attached to this HDF5 object.
///\return	Number of attributes
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
int H5Object::getNumAttrs() const
{
   H5O_info_t oinfo;    /* Object info */

   if(H5Oget_info(getId(), &oinfo) < 0)
      throw AttributeIException(inMemFunc("getNumAttrs"), "H5Oget_info failed");
   else
      return(static_cast<int>(oinfo.num_attrs));
}

//--------------------------------------------------------------------------
// Function:	H5Object::attrExists
///\brief	Checks whether the named attribute exists at this location.
///\param	name - IN: Name of the attribute to be queried
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - 2013
//--------------------------------------------------------------------------
bool H5Object::attrExists(const char* name) const
{
   // Call C routine H5Aexists to determine whether an attribute exists
   // at this location, which could be specified by a file, group, dataset,
   // or named datatype.
   herr_t ret_value = H5Aexists(getId(), name);
   if( ret_value > 0 )
      return true;
   else if(ret_value == 0)
      return false;
   else // Raise exception when H5Aexists returns a negative value
      throw AttributeIException(inMemFunc("attrExists"), "H5Aexists failed");
}

//--------------------------------------------------------------------------
// Function:	H5Object::attrExists
///\brief	This is an overloaded member function, provided for convenience.
///		It differs from the above function in that it takes
///		a reference to an \c H5std_string for \a name.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
bool H5Object::attrExists(const H5std_string& name) const
{
   return(attrExists(name.c_str()));
}

//--------------------------------------------------------------------------
// Function:	H5Object::removeAttr
///\brief	Removes the named attribute from this object.
///\param	name - IN: Name of the attribute to be removed
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
void H5Object::removeAttr( const char* name ) const
{
   herr_t ret_value = H5Adelete(getId(), name);
   if( ret_value < 0 )
      throw AttributeIException(inMemFunc("removeAttr"), "H5Adelete failed");
}

//--------------------------------------------------------------------------
// Function:	H5Object::removeAttr
///\brief	This is an overloaded member function, provided for convenience.
///		It differs from the above function in that it takes
///		a reference to an \c H5std_string for \a name.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
void H5Object::removeAttr( const H5std_string& name ) const
{
   removeAttr( name.c_str() );
}

//--------------------------------------------------------------------------
// Function:	H5Object::renameAttr
///\brief	Renames the named attribute from this object.
///\param	oldname - IN: Name of the attribute to be renamed
///\param	newname - IN: New name ame of the attribute
///\exception	H5::AttributeIException
// Programmer	Binh-Minh Ribler - Mar, 2005
//--------------------------------------------------------------------------
void H5Object::renameAttr(const char* oldname, const char* newname) const
{
   herr_t ret_value = H5Arename(getId(), oldname, newname);
   if (ret_value < 0)
      throw AttributeIException(inMemFunc("renameAttr"), "H5Arename failed");
}

//--------------------------------------------------------------------------
// Function:	H5Object::renameAttr
///\brief	This is an overloaded member function, provided for convenience.
///		It differs from the above function in that it takes
///		a reference to an \c H5std_string for the names.
// Programmer	Binh-Minh Ribler - Mar, 2005
//--------------------------------------------------------------------------
void H5Object::renameAttr(const H5std_string& oldname, const H5std_string& newname) const
{
   renameAttr (oldname.c_str(), newname.c_str());
}
//--------------------------------------------------------------------------
// Function:    getObjName
///\brief       Given an id, returns the type of the object.
///\return      The name of the object
// Programmer   Binh-Minh Ribler - Mar, 2014
//--------------------------------------------------------------------------
ssize_t H5Object::getObjName(char *obj_name, size_t buf_size) const
{
    // H5Iget_name will get buf_size-1 chars of the name to null terminate it
    ssize_t name_size = H5Iget_name(getId(), obj_name, buf_size);

    // If H5Iget_name returns a negative value, raise an exception
    if (name_size < 0)
    {
        throw Exception(inMemFunc("getObjName"), "H5Iget_name failed");
    }
    else if (name_size == 0)
    {
        throw Exception(inMemFunc("getObjName"), "Object must have a name, but name length is 0");
    }
    // Return length of the name
    return(name_size);
}

//--------------------------------------------------------------------------
// Function:    H5Object::getObjName
///\brief       Returns the name of this object as an \a H5std_string.
///\return      Name of the object
///\exception   H5::Exception
// Programmer   Binh-Minh Ribler - Mar, 2014
// Modification
//--------------------------------------------------------------------------
H5std_string H5Object::getObjName() const
{
    H5std_string obj_name(""); // object name to return

    // Preliminary call to get the size of the object name
    ssize_t name_size = H5Iget_name(getId(), NULL, static_cast<size_t>(0));

    // If H5Iget_name failed, throw exception
    if (name_size < 0)
    {
        throw Exception(inMemFunc("getObjName"), "H5Iget_name failed");
    }
    else if (name_size == 0)
    {
        throw Exception(inMemFunc("getObjName"), "Object must have a name, but name length is 0");
    }
    // Object's name exists, retrieve it
    else if (name_size > 0)
    {
        char* name_C = new char[name_size+1];  // temporary C-string
        HDmemset(name_C, 0, name_size+1); // clear buffer

        // Use overloaded function
        name_size = getObjName(name_C, name_size+1);

        // Convert the C object name to return
        obj_name = name_C;

        // Clean up resource
        delete []name_C;
    }
    // Return object's name
    return(obj_name);
}

//--------------------------------------------------------------------------
// Function:    H5Object::getObjName
///\brief       Gets the name of this object, returning its length.
///\param       obj_name - OUT: Buffer for the name string as \a H5std_string
///\param       len  -  IN: Desired length of the name, default to 0
///\return      Actual length of the object name
///\exception   H5::Exception
///\par Description
///             This function retrieves the object's name as an std string.
///             buf_size can specify a specific length or default to 0, in
///             which case the entire name will be retrieved.
// Programmer   Binh-Minh Ribler - Mar, 2014
//--------------------------------------------------------------------------
ssize_t H5Object::getObjName(H5std_string& obj_name, size_t len) const
{
    ssize_t name_size = 0;

    // If no length is provided, get the entire object name
    if (len == 0)
    {
        obj_name = getObjName();
        name_size = obj_name.length();
    }
    // If length is provided, get that number of characters in name
    else
    {
        char* name_C = new char[len+1];  // temporary C-string
        HDmemset(name_C, 0, len+1); // clear buffer

        // Use overloaded function
        name_size = getObjName(name_C, len+1);

        // Convert the C object name to return
        obj_name = name_C;

        // Clean up resource
        delete []name_C;
    }
    // Otherwise, keep obj_name intact

    // Return name size
    return(name_size);
}

#ifndef DOXYGEN_SHOULD_SKIP_THIS
//--------------------------------------------------------------------------
// Function:	H5Object destructor
///\brief	Noop destructor.
// Programmer	Binh-Minh Ribler - 2000
//--------------------------------------------------------------------------
H5Object::~H5Object() {}
#endif // DOXYGEN_SHOULD_SKIP_THIS

} // end namespace
