/*------------------------------------------------------------------------------
 * TypeMap.c
 *----------------------------------------------------------------------------*/
/**
 * Copyright � Peter Wood, 2005
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 *
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for
 * the specificlanguage governing rights and  limitations under the License.
 *
 * The Original Code is the FireRuby extension for the Ruby language.
 *
 * The Initial Developer of the Original Code is Peter Wood. All Rights
 * Reserved.
 *
 * @author  Peter Wood
 * @version 1.0
 */

/* Includes. */
#include "TypeMap.h"
#include <time.h>
#include <math.h>
#include <limits.h>
#include "Common.h"
#include "Blob.h"
#include "Connection.h"
#include "Transaction.h"
#include "Statement.h"
#include "FireRuby.h"
#include "rfbint.h"
#include "rfbstr.h"

/* Function prototypes. */
VALUE createDate(const struct tm *);
VALUE createDateTime(VALUE dt);
VALUE createTime(VALUE dt);
VALUE createSafeTime(const struct tm*);
VALUE toDateTime(VALUE);
VALUE rescueConvert(VALUE, VALUE);
void storeBlob(VALUE, XSQLVAR *, ConnectionHandle *, TransactionHandle *);
void populateBlobField(VALUE, XSQLVAR *, VALUE, VALUE);
void populateDoubleField(VALUE, XSQLVAR *);
void populateFloatField(VALUE, XSQLVAR *);
void populateInt64Field(VALUE, XSQLVAR *);
void populateLongField(VALUE, XSQLVAR *);
void populateShortField(VALUE, XSQLVAR *);
void populateTextField(VALUE, XSQLVAR *);
void populateDateField(VALUE, XSQLVAR *);
void populateTimeField(VALUE, XSQLVAR *);
void populateTimestampField(VALUE, XSQLVAR *);

static ID 
  RB_INTERN_NEW,
  RB_INTERN_TO_F,
  RB_INTERN_ROUND,
  RB_INTERN_ASTERISK,
  RB_INTERN_CLASS,
  RB_INTERN_NAME;
  
static VALUE cDate, cDateTime;

long long sql_scale(VALUE value, XSQLVAR *field) {
  value = rb_funcall(value, RB_INTERN_TO_F, 0);
  if(field->sqlscale) {
    // this requires special care - decimal point shift can cause type overflow
    // the easyest way is to use ruby arithmetics (although it's not the fastes)
    value = rb_funcall(value, RB_INTERN_ASTERISK, 1, LONG2NUM((long)pow(10, abs(field->sqlscale))));
  }
  return NUM2LL(rb_funcall(value, RB_INTERN_ROUND, 0));
}

VALUE sql_unscale(VALUE value, XSQLVAR *field) {
  if(field->sqlscale == 0) {
    return value;
  }
  return rb_float_new(
    NUM2DBL(rb_funcall(value, RB_INTERN_TO_F, 0)) / pow(10, abs(field->sqlscale))
  );
}

/**
 * This function converts a single XSQLVAR entry to a Ruby VALUE type.
 *
 * @param  entry        A pointer to the SQLVAR type containing the data to be
 *                      converted.
 * @param  connection   The connection object relating to the data.
 * @param  transaction  The transaction handle relating to the data.
 *
 * @return  A Ruby type for the XSQLVAR entry. The actual type will depend on
 *          the field type referenced.
 *
 */
VALUE toValue(XSQLVAR *entry,
              VALUE connection, 
              VALUE transaction) {
  VALUE value = Qnil;

  /* Check for NULL values. */
  if(!((entry->sqltype & 1) && (*entry->sqlind < 0))) {
    int type    = (entry->sqltype & ~1);
    char       *array  = NULL,
                column[256],
                table[256];
    struct tm datetime;
    short length;
    double actual;
    BlobHandle *blob   = NULL;
    VALUE working = Qnil;

    switch(type) {
    case SQL_BLOB:        /* Type: BLOB */
      memset(column, 0, 256);
      memset(table, 0, 256);
      memcpy(column, entry->sqlname, entry->sqlname_length);
      memcpy(table, entry->relname, entry->relname_length);
      blob = openBlob(entry, column, table, connection,
                      transaction);
      working = Data_Wrap_Struct(cBlob, NULL, blobFree, blob);
      value = initializeBlob(working, connection);
      break;

    case SQL_TYPE_DATE:       /* Type: DATE */
      memset(&datetime, 0, sizeof(struct tm));
      isc_decode_sql_date((ISC_DATE *)entry->sqldata, &datetime);
      datetime.tm_sec  = 0;
      datetime.tm_min  = 0;
      datetime.tm_hour = 0;
      if(getFireRubySetting("DATE_AS_DATE") == Qtrue) {
        value = createDate(&datetime);
      } else {
        value = createSafeTime(&datetime);
      }
      break;

    case SQL_DOUBLE:       /* Type: DOUBLE PRECISION, DECIMAL, NUMERIC */
      value = rb_float_new(*((double *)entry->sqldata));
      break;

    case SQL_FLOAT:       /* Type: FLOAT */
      value =  rb_float_new(*((float *)entry->sqldata));
      break;

    case SQL_INT64:       /* Type: DECIMAL, NUMERIC */
      value =  sql_unscale(LL2NUM(*((long long *)entry->sqldata)), entry);
      break;

    case SQL_LONG:       /* Type: INTEGER, DECIMAL, NUMERIC */
      value = sql_unscale(LONG2NUM(*((int32_t *)entry->sqldata)), entry);
      break;

    case SQL_SHORT:       /* Type: SMALLINT, DECIMAL, NUMERIC */
      value = sql_unscale(INT2NUM(*((short *)entry->sqldata)), entry);
      break;

    case SQL_TEXT:       /* Type: CHAR */
      value = rfbstr(connection, entry->sqlsubtype, entry->sqldata, entry->sqllen);
      break;

    case SQL_TYPE_TIME:       /* Type: TIME */
      isc_decode_sql_time((ISC_TIME *)entry->sqldata, &datetime);
      datetime.tm_year = 70;
      datetime.tm_mon  = 0;
      datetime.tm_mday = 1;
      value = createSafeTime(&datetime);
      break;

    case SQL_TIMESTAMP:       /* Type: TIMESTAMP */
      isc_decode_timestamp((ISC_TIMESTAMP *)entry->sqldata, &datetime);
      value = createSafeTime(&datetime);
      break;

    case SQL_VARYING:
      memcpy(&length, entry->sqldata, 2);
      value = rfbstr(connection, entry->sqlsubtype, &entry->sqldata[2], length);
      break;
    }   /* End of switch. */
  }

  return(value);
}


/**
 * This function takes an array of parameters and populates the parameter set
 * for a Statement object with the details. Attempts are made to convert the
 * parameter types where appropriate but, if this isn't possible then an
 * exception is generated.
 *
 * @param  parameters  A pointer to the XSQLDA area that will be used to
 *                     hold the parameter data.
 * @param  array       A reference to an array containing the parameter data to
 *                     be used.
 * @param  source      Either a Statement or ResultSet object that can be used
 *                     to get connection and transaction details.
 *
 */
void setParameters(XSQLDA *parameters, VALUE array, VALUE transaction, VALUE connection) {
  long index,
       size;
  XSQLVAR *parameter = NULL;

  /* Check that sufficient parameters have been provided. */
  size      = RARRAY_LEN(array);
  parameter = parameters->sqlvar;
  if(size != parameters->sqld) {
    rb_raise(rb_eException,
             "Parameter set mismatch. Too many or too few parameters " \
             "specified for a SQL statement.");
  }
  parameters->sqln = parameters->sqld;
  parameters->version = 1;

  /* Populate the parameters from the array's contents. */
  for(index = 0; index < size; index++, parameter++) {
    int type = (parameter->sqltype & ~1);
    VALUE value = rb_ary_entry(array, index);

    /* Check for nils to indicate null values. */
    if(value != Qnil) {
      VALUE name = rb_funcall(value, RB_INTERN_CLASS, 0);

      *parameter->sqlind = 0;
      name = rb_funcall(name, RB_INTERN_NAME, 0);
      switch(type) {
      case SQL_ARRAY:        /* Type: ARRAY */
        /* TO BE DONE! */
        break;

      case SQL_BLOB:         /* Type: BLOB */
        populateBlobField(value, parameter, transaction, connection);
        break;

      case SQL_DOUBLE:        /* Type: DOUBLE PRECISION, DECIMAL, NUMERIC */
        populateDoubleField(value, parameter);
        break;

      case SQL_FLOAT:        /* Type: FLOAT */
        populateFloatField(value, parameter);
        break;

      case SQL_INT64:        /* Type: DECIMAL, NUMERIC */
        populateInt64Field(value, parameter);
        break;

      case SQL_LONG:        /* Type: INTEGER, DECIMAL, NUMERIC */
        populateLongField(value, parameter);
        break;

      case SQL_SHORT:        /* Type: SMALLINT, DECIMAL, NUMERIC */
        populateShortField(value, parameter);
        break;

      case SQL_TEXT:        /* Type: CHAR */
        populateTextField(value, parameter);
        break;

      case SQL_TYPE_DATE:        /* Type: DATE */
        populateDateField(value, parameter);
        break;

      case SQL_TYPE_TIME:        /* Type: TIME */
        populateTimeField(value, parameter);
        break;

      case SQL_TIMESTAMP:        /* Type: TIMESTAMP */
        populateTimestampField(value, parameter);
        break;

      case SQL_VARYING:        /* Type: VARCHAR */
        populateTextField(value, parameter);
        break;

      default:
        rb_raise(rb_eException,
                 "Unknown SQL type encountered in statement parameter " \
                 "set.");
      }    /* End of the switch statement. */
    } else {
      /* Mark the field as a NULL value. */
      memset(parameter->sqldata, 0, parameter->sqllen);
      *parameter->sqlind = -1;
    }
  }
}


/**
 * This function converts a struct tm to a Ruby Date instance.
 *
 * @param  date  A structure containing the date details.
 *
 * @return  A Ruby Date object.
 *
 */
VALUE createDate(const struct tm *date) {
  return rb_funcall(cDate, RB_INTERN_NEW, 3, INT2FIX(date->tm_year + 1900), INT2FIX(date->tm_mon + 1), INT2FIX(date->tm_mday));
}


/**
 * This function converts a struct tm to a Ruby DateTime instance.
 *
 * @param  datetime  A structure containing the date/time details.
 *
 * @return  A Ruby DateTime object.
 *
 */
VALUE createDateTime(VALUE dt) {
  struct tm *datetime;
  Data_Get_Struct(dt, struct tm, datetime);

  return rb_funcall(cDateTime, RB_INTERN_NEW, 7, 
    INT2FIX(datetime->tm_year + 1900),
    INT2FIX(datetime->tm_mon + 1),
    INT2FIX(datetime->tm_mday),
    INT2FIX(datetime->tm_hour),
    INT2FIX(datetime->tm_min),
    INT2FIX(datetime->tm_sec),
    rb_funcall(rb_funcall(cDateTime, rb_intern("now"), 0), rb_intern("offset"), 0)
  );
}


/**
 * This function converts a struct tm to a Ruby Time instance.
 *
 * @param  datetime  A structure containing the date/time details.
 *
 * @return  A Ruby Time object.
 *
 */
VALUE createTime(VALUE dt) {
  struct tm *datetime;
  Data_Get_Struct(dt, struct tm, datetime);

  /*fprintf(stderr, "%d-%d-%d %d:%d:%d\n", datetime->tm_year + 1900,
          datetime->tm_mon + 1, datetime->tm_mday, datetime->tm_hour,
          datetime->tm_min, datetime->tm_sec);*/

  /* Create the class instance. */
  return rb_funcall(rb_cTime, rb_intern("local"), 6, 
    INT2FIX(datetime->tm_year + 1900),
    INT2FIX(datetime->tm_mon + 1),
    INT2FIX(datetime->tm_mday),
    INT2FIX(datetime->tm_hour),
    INT2FIX(datetime->tm_min),
    INT2FIX(datetime->tm_sec)
  );
}

/**
 * This function converts a struct tm to a Ruby Time instance.
 * If the conversion process results in an out of range error then
 * it will convert the struct to a DateTime instance.
 *
 * @param datetime  A structure containing the date/time details.
 *
 * @return  A Ruby Time object if the arguments are in range, otherwise
 *          a Ruby DateTime object.
 *
 */
VALUE createSafeTime(const struct tm *datetime) {
  VALUE dt = Data_Wrap_Struct(rb_cObject, NULL, NULL, (void*)datetime);
  return rb_rescue(createTime, dt, createDateTime, dt);
}

/**
 * This function attempts to convert a VALUE to a series of date/time values.
 *
 * @param  value  A reference to the value to be converted.
 *
 * @return  A VALUE that represents an array containing the date/time details
 *          in the order year, month, day of month, hours, minutes and seconds.
 *
 */
VALUE toDateTime(VALUE value) {
  VALUE result,
        klass = rb_funcall(value, RB_INTERN_CLASS, 0);

  if((klass == rb_cTime) || (klass == cDateTime)) {
    VALUE data;

    if (klass == cDateTime) {
      value = rb_funcall(value, rb_intern("to_time"), 0);
    }
    result = rb_ary_new();

    data = rb_funcall(value, rb_intern("year"), 0);
    rb_ary_push(result, INT2FIX(FIX2INT(data) - 1900));
    data = rb_funcall(value, rb_intern("month"), 0);
    rb_ary_push(result, INT2FIX(FIX2INT(data) - 1));
    rb_ary_push(result, rb_funcall(value, rb_intern("day"), 0));
    rb_ary_push(result, rb_funcall(value, rb_intern("hour"), 0));
    rb_ary_push(result, rb_funcall(value, rb_intern("min"), 0));
    rb_ary_push(result, rb_funcall(value, rb_intern("sec"), 0));
  } else if(klass == cDate) {
    VALUE data;

    result = rb_ary_new();

    data = rb_funcall(value, rb_intern("year"), 0);
    rb_ary_push(result, INT2FIX(FIX2INT(data) - 1900));
    data = rb_funcall(value, rb_intern("month"), 0);
    rb_ary_push(result, INT2FIX(FIX2INT(data) - 1));
    rb_ary_push(result, rb_funcall(value, rb_intern("mday"), 0));
    rb_ary_push(result, INT2FIX(0));
    rb_ary_push(result, INT2FIX(0));
    rb_ary_push(result, INT2FIX(0));
  } else {
    rb_raise(rb_eException, "Value conversion error.");
  }

  return(result);
}


/**
 * This function represents the rescue block for data conversions.
 *
 * @param  arguments  An array of VALUEs. The first is expected to contain the
 *                    field offset of the value being converted. The second a
 *                    string containing the name of the data type being
 *                    converted from and the third a string containing the name
 *                    of the data type being converted to.
 * @param  error      Will be populates with error details of the exception
 *                    raised.
 *
 */
VALUE rescueConvert(VALUE arguments, VALUE error) {
  VALUE message,
        tmp_str1 = Qnil,
        tmp_str2 = Qnil;
  char text[512];

  tmp_str1 = rb_ary_entry(arguments, 1);
  tmp_str2 = rb_ary_entry(arguments, 2);
  sprintf(text, "Error converting input column %d from a %s to a %s.",
          FIX2INT(rb_ary_entry(arguments, 0)),
          StringValuePtr(tmp_str1),
          StringValuePtr(tmp_str2));
  message = rb_str_new2(text);

  return(rb_funcall(rb_eException, rb_intern("exception"), 1, &message));
}

long getLongProperty(VALUE obj, const char* name) {
  VALUE number  = rb_funcall(obj, rb_intern(name), 0);
  return TYPE(number) == T_FIXNUM ? FIX2INT(number) : NUM2INT(number);
}

/**
 * This function creates a new blob and returns the identifier for it.
 *
 * @param  info         A pointer to a string containing the blob data.
 * @param  field        The field that the blob identifier needs to be inserted
 *                      into.
 * @param  database     A pointer to the database handle to be used in creating
 *                      the blob.
 * @param  transaction  A pointer to the transaction handle to be used in
 *                      creating the blob.
 *
 * @return  An ISC_QUAD value containing the identifier for the blob created.
 *
 */
void storeBlob(VALUE info,
               XSQLVAR *field,
               ConnectionHandle *connection,
               TransactionHandle *transaction) {
  ISC_STATUS status[ISC_STATUS_LENGTH];
  isc_blob_handle handle  = 0;
  ISC_QUAD        *blobId = (ISC_QUAD *)field->sqldata;
  char *data   = StringValuePtr(info);
  long dataLength = getLongProperty(info, "length");

  if(Qtrue == rb_funcall(info, rb_intern("respond_to?"), 1, ID2SYM(rb_intern("bytesize")))) {
    /* 1.9 strings */
    dataLength = getLongProperty(info, "bytesize");
  }

  field->sqltype = SQL_BLOB;

  if(isc_create_blob(status, &connection->handle, &transaction->handle,
                     &handle, blobId) == 0) {
    long offset = 0;
    unsigned short size   = 0;

    while(offset < dataLength) {
      char *buffer = &data[offset];

      size = (dataLength - offset) > USHRT_MAX ? USHRT_MAX : dataLength - offset;
      if(isc_put_segment(status, &handle, size, buffer) != 0) {
        ISC_STATUS other[20];

        isc_close_blob(other, &handle);
        rb_fireruby_raise(status, "Error writing blob data.");
      }

      offset = offset + size;
    }

    if(isc_close_blob(status, &handle) != 0) {
      rb_fireruby_raise(status, "Error closing blob.");
    }
  } else {
    rb_fireruby_raise(status, "Error storing blob data.");
  }
}


/**
 * This function populates a blob output parameter.
 *
 * @param  value   The value to be insert into the blob.
 * @param  field   A pointer to the output field to be populated.
 * @param  source  A reference to either a Statement or ResultSet object that
 *                 contains the connection and transaction details.
 *
 */
void populateBlobField(VALUE value, XSQLVAR *field, VALUE transaction, VALUE connection) {
  ConnectionHandle  *hConnection = NULL;
  TransactionHandle *hTransaction = NULL;

  if(TYPE(value) != T_STRING) {
    rb_fireruby_raise(NULL, "Error converting input parameter to blob.");
  }

  /* Fetch the connection and transaction details. */
  Data_Get_Struct(connection, ConnectionHandle, hConnection);
  Data_Get_Struct(transaction, TransactionHandle, hTransaction);
  storeBlob(value, field, hConnection, hTransaction);
  field->sqltype = SQL_BLOB;
}


/**
 * This method populates a date output field.
 *
 * @param  value  A reference to the value to be used to populate the field.
 * @param  field  A pointer to the output field to be populated.
 *
 */
void populateDateField(VALUE value, XSQLVAR *field) {
  struct tm datetime;
  VALUE arguments = rb_ary_new();

  rb_ary_push(arguments, rb_str_new2("date"));
  value = rb_rescue(toDateTime, value, rescueConvert, arguments);
  if(TYPE(value) != T_ARRAY) {
    VALUE message = rb_funcall(value, rb_intern("message"), 0);
    rb_fireruby_raise(NULL, StringValuePtr(message));
  }
  datetime.tm_year = FIX2INT(rb_ary_entry(value, 0));
  datetime.tm_mon  = FIX2INT(rb_ary_entry(value, 1));
  datetime.tm_mday = FIX2INT(rb_ary_entry(value, 2));
  isc_encode_sql_date(&datetime, (ISC_DATE *)field->sqldata);
  field->sqltype   = SQL_TYPE_DATE;
}


/**
 * This method populates a double output field.
 *
 * @param  value  A reference to the value to be used to populate the field.
 * @param  field  A pointer to the output field to be populated.
 *
 */
void populateDoubleField(VALUE value, XSQLVAR *field) {
  double store;
  VALUE actual;

  if(TYPE(value) != T_FLOAT) {
    if(rb_obj_is_kind_of(value, rb_cNumeric) || TYPE(value) == T_STRING) {
      actual = rb_funcall(value, RB_INTERN_TO_F, 0);
    } else {
      rb_fireruby_raise(NULL,
                        "Error converting input parameter to double.");
    }
  }

  store = NUM2DBL(value);
  memcpy(field->sqldata, &store, field->sqllen);
  field->sqltype = SQL_DOUBLE;
}


/**
 * This method populates a double output field.
 *
 * @param  value  A reference to the value to be used to populate the field.
 * @param  field  A pointer to the output field to be populated.
 *
 */
void populateFloatField(VALUE value, XSQLVAR *field) {
  double full  = 0.0;
  float store = 0.0;
  VALUE actual;

  if(TYPE(value) != T_FLOAT) {
    if(rb_obj_is_kind_of(value, rb_cNumeric) || TYPE(value) == T_STRING) {
      actual = rb_funcall(value, RB_INTERN_TO_F, 0);
    } else {
      rb_fireruby_raise(NULL,
                        "Error converting input parameter to double.");
    }
  }

  full = NUM2DBL(value);
  store = (float)full;
  memcpy(field->sqldata, &store, field->sqllen);
  field->sqltype = SQL_FLOAT;
}


/**
 * This function populates a output parameter field with the data of a Ruby
 * value.
 *
 * @param  value  A reference to the Ruby value to be inserted into the field.
 * @param  field  A pointer to the XSQLVAR field that the value will go into.
 *
 */
void populateInt64Field(VALUE value, XSQLVAR *field) {
  *((long long *)field->sqldata) = sql_scale(value, field);
  field->sqltype = SQL_INT64;
}


/**
 * This function populates a output parameter field with the data of a Ruby
 * value.
 *
 * @param  value  A reference to the Ruby value to be inserted into the field.
 * @param  field  A pointer to the XSQLVAR field that the value will go into.
 *
 */
void populateLongField(VALUE value, XSQLVAR *field) {
  *((long *)field->sqldata) = sql_scale(value, field);
  field->sqltype = SQL_LONG;
}


/**
 * This function populates a output parameter field with the data of a Ruby
 * value.
 *
 * @param  value  A reference to the Ruby value to be inserted into the field.
 * @param  field  A pointer to the XSQLVAR field that the value will go into.
 *
 */
void populateShortField(VALUE value, XSQLVAR *field) {
  *((short *)field->sqldata) = sql_scale(value, field);
  field->sqltype = SQL_SHORT;
}


/**
 * This function populates a output parameter field with the data of a Ruby
 * value.
 *
 * @param  value  A reference to the Ruby value to be inserted into the field.
 * @param  field  A pointer to the XSQLVAR field that the value will go into.
 *
 */
void populateTextField(VALUE value, XSQLVAR *field) {
  VALUE actual;
  char  *text  = NULL;
  short length = 0;

  if(TYPE(value) == T_STRING) {
    actual = value;
  } else {
    actual = rb_funcall(value, rb_intern("to_s"), 0);
  }

  text   = StringValuePtr(actual);
  length = strlen(text) > field->sqllen ? field->sqllen : strlen(text);

  if((field->sqltype & ~1) == SQL_TEXT) {
    memcpy(field->sqldata, text, length);
    field->sqltype = SQL_TEXT;
  } else {
    memcpy(field->sqldata, &length, sizeof(short));
    memcpy(&field->sqldata[sizeof(short)], text, length);
    field->sqltype = SQL_VARYING;
  }
  field->sqllen = length;
}


/**
 * This method populates a date output field.
 *
 * @param  value  A reference to the value to be used to populate the field.
 * @param  field  A pointer to the output field to be populated.
 *
 */
void populateTimeField(VALUE value, XSQLVAR *field) {
  struct tm datetime;
  VALUE arguments = rb_ary_new();

  rb_ary_push(arguments, rb_str_new2("time"));
  value = rb_rescue(toDateTime, value, rescueConvert, arguments);
  if(TYPE(value) != T_ARRAY) {
    VALUE message = rb_funcall(value, rb_intern("message"), 0);
    rb_fireruby_raise(NULL, StringValuePtr(message));
  }
  datetime.tm_hour = FIX2INT(rb_ary_entry(value, 3));
  datetime.tm_min  = FIX2INT(rb_ary_entry(value, 4));
  datetime.tm_sec  = FIX2INT(rb_ary_entry(value, 5));
  isc_encode_sql_time(&datetime, (ISC_TIME *)field->sqldata);
  field->sqltype   = SQL_TYPE_TIME;
}

/**
 * This method populates a date output field.
 *
 * @param  value  A reference to the value to be used to populate the field.
 * @param  field  A pointer to the output field to be populated.
 *
 */
void populateTimestampField(VALUE value, XSQLVAR *field) {
  struct tm datetime;
  VALUE arguments = rb_ary_new();

  rb_ary_push(arguments, rb_str_new2("timestamp"));
  value = rb_rescue(toDateTime, value, rescueConvert, arguments);
  if(TYPE(value) != T_ARRAY) {
    VALUE message = rb_funcall(value, rb_intern("message"), 0);
    rb_fireruby_raise(NULL, StringValuePtr(message));
  }
  datetime.tm_year = FIX2INT(rb_ary_entry(value, 0));
  datetime.tm_mon  = FIX2INT(rb_ary_entry(value, 1));
  datetime.tm_mday = FIX2INT(rb_ary_entry(value, 2));
  datetime.tm_hour = FIX2INT(rb_ary_entry(value, 3));
  datetime.tm_min  = FIX2INT(rb_ary_entry(value, 4));
  datetime.tm_sec  = FIX2INT(rb_ary_entry(value, 5));
  isc_encode_timestamp(&datetime, (ISC_TIMESTAMP *)field->sqldata);
  field->sqltype   = SQL_TIMESTAMP;
}

void Init_TypeMap(VALUE module) {
  RB_INTERN_NEW = rb_intern("new");
  RB_INTERN_TO_F = rb_intern("to_f");
  RB_INTERN_ROUND = rb_intern("round");
  RB_INTERN_ASTERISK = rb_intern("*");
  RB_INTERN_CLASS = rb_intern("class");
  RB_INTERN_NAME = rb_intern("name");
  
  cDate = getClass("Date");
  cDateTime = getClass("DateTime");
}
