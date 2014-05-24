/*
 * config.c - application independent configuration handling 
 * This file is part of the TinyG2 project
 *
 * Copyright (c) 2010 - 2014 Alden S. Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 *	See config.h for a Config system overview and a bunch of details.
 */

#include "tinyg.h"		// #1
#include "config.h"		// #2
#include "report.h"
#include "controller.h"
#include "canonical_machine.h"
#include "json_parser.h"
#include "text_parser.h"
#include "persistence.h"
#include "hardware.h"
#include "help.h"
#include "util.h"
#include "xio.h"

#ifdef __cplusplus
extern "C"{
#endif

/***********************************************************************************
 **** STRUCTURE ALLOCATIONS ********************************************************
 ***********************************************************************************/

nvStr_t nvStr;
nvObj_t nv_list[NV_LIST_LEN];	// JSON header element

/***********************************************************************************
 **** CODE *************************************************************************
 ***********************************************************************************/
/* Primary access points to cmd functions
 * These gatekeeper functions check index ranges so others don't have to
 *
 * nv_set() 	- Write a value or invoke a function - operates on single valued elements or groups
 * nv_get() 	- Build a cmdObj with the values from the target & return the value
 *			   	  Populate cmd body with single valued elements or groups (iterates)
 * nv_print()	- Output a formatted string for the value.
 * nv_persist()- persist value to NVM. Takes special cases into account
 */
stat_t nv_set(nvObj_t *nv)
{
	if (nv->index >= nv_index_max()) { return(STAT_INTERNAL_RANGE_ERROR);}
	return (((fptrCmd)GET_TABLE_WORD(set))(nv));
}

stat_t nv_get(nvObj_t *nv)
{
	if (nv->index >= nv_index_max()) { return(STAT_INTERNAL_RANGE_ERROR);}
	return (((fptrCmd)GET_TABLE_WORD(get))(nv));
}

void nv_print(nvObj_t *nv)
{
	if (nv->index >= nv_index_max()) return;
	((fptrCmd)GET_TABLE_WORD(print))(nv);
}

void nv_persist(nvObj_t *nv)
{
#ifdef __DISABLE_PERSISTENCE	// cutout for faster simulation in test
	return;
#endif
	if (nv_index_lt_groups(nv->index) == false) return;
	if (GET_TABLE_BYTE(flags) & F_PERSIST) write_persistent_value(nv);
}

/************************************************************************************
 * config_init()  - called once on hard reset
 *
 * Performs one of 2 actions:
 *	(1) if NVM is set up or out-of-rev load RAM and NVM with settings.h defaults
 *	(2) if NVM is set up and at current config version use NVM data for config
 *
 *	You can assume the cfg struct has been zeroed by a hard reset. 
 *	Do not clear it as the version and build numbers have already been set by tg_init()
 *
 * NOTE: Config assertions are handled from the controller
 */
void config_init()
{
	nvObj_t *nv = nv_reset_nvObj_list();
	nvStr.magic_start = MAGICNUM;
	nvStr.magic_end = MAGICNUM;
	cfg.magic_start = MAGICNUM;
	cfg.magic_end = MAGICNUM;

	cm_set_units_mode(MILLIMETERS);			// must do inits in MM mode
	nv->index = 0;							// this will read the first record in NVM

	read_persistent_value(nv);
	if (nv->value != cs.fw_build) {
		nv->value = true;					// case (1) NVM is not setup or not in revision
		set_defaults(nv);
	} else {								// case (2) NVM is setup and in revision
		rpt_print_loading_configs_message();
		for (nv->index=0; nv_index_is_single(nv->index); nv->index++) {
			if (GET_TABLE_BYTE(flags) & F_INITIALIZE) {
				strncpy_P(nv->token, cfgArray[nv->index].token, TOKEN_LEN);	// read the token from the array
				read_persistent_value(nv);
				nv_set(nv);
			}
		}
		sr_init_status_report();
	}
}

/*
 * set_defaults() - reset NVM with default values for active profile
 */
stat_t set_defaults(nvObj_t *nv) 
{
	if (fp_FALSE(nv->value)) {				// failsafe. Must set true or no action occurs
		help_defa(nv);
		return (STAT_OK);
	}
	cm_set_units_mode(MILLIMETERS);			// must do inits in MM mode

	for (nv->index=0; nv_index_is_single(nv->index); nv->index++) {
		if (GET_TABLE_BYTE(flags) & F_INITIALIZE) {
			nv->value = GET_TABLE_FLOAT(def_value);
			strncpy_P(nv->token, cfgArray[nv->index].token, TOKEN_LEN);
			nv_set(nv);
			nv_persist(nv);				// persist must occur when no other interrupts are firing
		}
	}
	rpt_print_initializing_message();		// don't start TX until all the NVM persistence is done
	sr_init_status_report();				// reset status reports
	return (STAT_OK);
}

/***** Generic Internal Functions *********************************************/

/* Generic gets()
 *	get_nul()  - get nothing (returns STAT_NOOP)
 *	get_ui8()  - get value as 8 bit uint8_t
 *	get_int()  - get value as 32 bit integer
 *	get_data() - get value as 32 bit integer blind cast
 *	get_flt()  - get value as float
 *	get_format() - internal accessor for printf() format string
 */
stat_t get_nul(nvObj_t *nv) 
{ 
	nv->valuetype = TYPE_NULL;
	return (STAT_NOOP);
}

stat_t get_ui8(nvObj_t *nv)
{
	nv->value = (float)*((uint8_t *)GET_TABLE_WORD(target));
	nv->valuetype = TYPE_INTEGER;
	return (STAT_OK);
}

stat_t get_int(nvObj_t *nv)
{
//	nv->value = (float)*((uint32_t *)GET_TABLE_WORD(target));
	nv->value = *((uint32_t *)GET_TABLE_WORD(target));
	nv->valuetype = TYPE_INTEGER;
	return (STAT_OK);
}

stat_t get_data(nvObj_t *nv)
{
	uint32_t *v = (uint32_t*)&nv->value;
	*v = *((uint32_t *)GET_TABLE_WORD(target));
	nv->valuetype = TYPE_DATA;
	return (STAT_OK);
}

stat_t get_flt(nvObj_t *nv)
{
	nv->value = *((float *)GET_TABLE_WORD(target));
	nv->precision = (int8_t)GET_TABLE_WORD(precision);
	nv->valuetype = TYPE_FLOAT;
	return (STAT_OK);
}

/* Generic sets()
 *	set_nul()  - set nothing (returns STAT_NOOP)
 *	set_ui8()  - set value as 8 bit uint8_t value
 *	set_01()   - set a 0 or 1 uint8_t value with validation
 *	set_012()  - set a 0, 1 or 2 uint8_t value with validation
 *	set_0123() - set a 0, 1, 2 or 3 uint8_t value with validation
 *	set_int()  - set value as 32 bit integer
 *	set_data() - set value as 32 bit integer blind cast 
 *	set_flt()  - set value as float
 */
stat_t set_nul(nvObj_t *nv) { return (STAT_NOOP);}

stat_t set_ui8(nvObj_t *nv)
{
	*((uint8_t *)GET_TABLE_WORD(target)) = nv->value;
	nv->valuetype = TYPE_INTEGER;
	return(STAT_OK);
}

stat_t set_01(nvObj_t *nv)
{
	if (nv->value > 1) return (STAT_INPUT_VALUE_UNSUPPORTED);	// if
	return (set_ui8(nv));										// else
}

stat_t set_012(nvObj_t *nv)
{
	if (nv->value > 2) return (STAT_INPUT_VALUE_UNSUPPORTED);	// if
	return (set_ui8(nv));										// else
}

stat_t set_0123(nvObj_t *nv)
{
	if (nv->value > 3) return (STAT_INPUT_VALUE_UNSUPPORTED);	// if
	return (set_ui8(nv));										// else
}

stat_t set_int(nvObj_t *nv)
{
//	*((uint32_t *)GET_TABLE_WORD(target)) = nv->value;
	*((uint32_t *)GET_TABLE_WORD(target)) = (uint32_t)nv->value;
	nv->valuetype = TYPE_INTEGER;
	return(STAT_OK);
}

stat_t set_data(nvObj_t *nv)
{
	uint32_t *v = (uint32_t*)&nv->value;
	*((uint32_t *)GET_TABLE_WORD(target)) = *v;
	nv->valuetype = TYPE_DATA;
	return(STAT_OK);
}

stat_t set_flt(nvObj_t *nv)
{
	*((float *)GET_TABLE_WORD(target)) = nv->value;
	nv->precision = GET_TABLE_WORD(precision);
	nv->valuetype = TYPE_FLOAT;
	return(STAT_OK);
}

/***** GCODE SPECIFIC EXTENSIONS TO GENERIC FUNCTIONS *****/

/*
 * get_flu() - get floating point number with G20/G21 units conversion
 *
 * The number 'getted' will be in internal canonical units (mm), which is  
 * returned in external units (inches or mm) 
 */

stat_t get_flu(nvObj_t *nv)
{
	return(get_flt(nv));
//	if (cm_get_units_mode(MODEL) == INCHES) {
//		nv->value *= INCHES_PER_MM;
//		nv->units = INCHES;
//	}
//	return (STAT_OK);
}

/*
 * set_flu() - set floating point number with G20/G21 units conversion
 *
 * The number 'setted' will have been delivered in external units (inches or mm).
 * It is written to the target memory location in internal canonical units (mm),
 * but the original nv->value is not changed so display works correctly.
 */

stat_t set_flu(nvObj_t *nv)
{
	if (cm_get_units_mode(MODEL) == INCHES) {		// if in inches...
		nv->value *= MM_PER_INCH;					// convert to canonical millimeter units
	}
	*((float *)GET_TABLE_WORD(target)) = nv->value;// write value as millimeters or degrees
	nv->precision = GET_TABLE_WORD(precision);
	nv->valuetype = TYPE_FLOAT;
	return(STAT_OK);
}

/************************************************************************************
 * Group operations
 *
 *	Group operations work on parent/child groups where the parent is one of:
 *	  axis group 			x,y,z,a,b,c
 *	  motor group			1,2,3,4
 *	  PWM group				p1
 *	  coordinate group		g54,g55,g56,g57,g58,g59,g92
 *	  system group			"sys" - a collection of otherwise unrelated variables
 *
 *	Text mode can only GET groups. For example:
 *	  $x					get all members of an axis group
 *	  $1					get all members of a motor group
 *	  $<grp>				get any named group from the above lists
 *
 *	In JSON groups are carried as parent / child objects & can get and set elements:
 *	  {"x":""}						get all X axis parameters
 *	  {"x":{"vm":""}}				get X axis velocity max 
 *	  {"x":{"vm":1000}}				set X axis velocity max
 *	  {"x":{"vm":"","fr":""}}		get X axis velocity max and feed rate 
 *	  {"x":{"vm":1000,"fr";900}}	set X axis velocity max and feed rate
 *	  {"x":{"am":1,"fr":800,....}}	set multiple or all X axis parameters
 */

/* 
 * get_grp() - read data from axis, motor, system or other group
 *
 *	get_grp() is a group expansion function that expands the parent group and returns 
 *	the values of all the children in that group. It expects the first cmdObj in the 
 *	cmdBody to have a valid group name in the token field. This first object will be set 
 *	to a TYPE_PARENT. The group field is left nul - as the group field refers to a parent 
 *	group, which this group has none.
 *
 *	All subsequent cmdObjs in the body will be populated with their values.
 *	The token field will be populated as will the parent name in the group field. 
 *
 *	The sys group is an exception where the children carry a blank group field, even though 
 *	the sys parent is labeled as a TYPE_PARENT.
 */

stat_t get_grp(nvObj_t *nv)
{
	char_t *parent_group = nv->token;		// token in the parent cmd object is the group
	char_t group[GROUP_LEN+1];				// group string retrieved from cfgArray child
	nv->valuetype = TYPE_PARENT;				// make first object the parent 
	for (index_t i=0; nv_index_is_single(i); i++) {
		strcpy_P(group, cfgArray[i].group);  // don't need strncpy as it's always terminated
		if (strcmp(parent_group, group) != 0) continue;
		(++nv)->index = i;
		nv_get_nvObj(nv);
	}
	return (STAT_OK);
}

/*
 * set_grp() - get or set one or more values in a group
 *
 *	This functions is called "_set_group()" but technically it's a getter and 
 *	a setter. It iterates the group children and either gets the value or sets
 *	the value for each depending on the nv->valuetype.
 *
 *	This function serves JSON mode only as text mode shouldn't call it.
 */

stat_t set_grp(nvObj_t *nv)
{
	if (cfg.comm_mode == TEXT_MODE) return (STAT_UNRECOGNIZED_COMMAND);
	for (uint8_t i=0; i<NV_MAX_OBJECTS; i++) {
		if ((nv = nv->nx) == NULL) break;
		if (nv->valuetype == TYPE_EMPTY) break;
		else if (nv->valuetype == TYPE_NULL)	// NULL means GET the value
			nv_get(nv);
		else {
			nv_set(nv);
			nv_persist(nv);
		}
	}
	return (STAT_OK);
}

/*
 * nv_group_is_prefixed() - hack
 *
 *	This little function deals with the exception cases that some groups don't use
 *	the parent token as a prefix to the child elements; SR being a good example.
 */
uint8_t nv_group_is_prefixed(char_t *group)
{
	if (strcmp("sr",group) == 0) return (false);
	if (strcmp("sys",group) == 0) return (false);
	return (true);
}

/***********************************************************************************
 ***** cmdObj functions ************************************************************
 ***********************************************************************************/

/***********************************************************************************
 * cmdObj helper functions and other low-level cmd helpers
 */

/* nv_get_index() - get index from mnenonic token + group
 *
 * nv_get_index() is the most expensive routine in the whole config. It does a 
 * linear table scan of the PROGMEM strings, which of course could be further 
 * optimized with indexes or hashing.
 */
index_t nv_get_index(const char_t *group, const char_t *token)
{
	char_t c;
	char_t str[TOKEN_LEN + GROUP_LEN+1];	// should actually never be more than TOKEN_LEN+1
	strncpy(str, group, GROUP_LEN+1);
	strncat(str, token, TOKEN_LEN+1);

	index_t i;
	index_t index_max = nv_index_max();

	for (i=0; i < index_max; i++) {
		if ((c = GET_TOKEN_BYTE(token[0])) != str[0]) {	continue; }					// 1st character mismatch
		if ((c = GET_TOKEN_BYTE(token[1])) == NUL) { if (str[1] == NUL) return(i);}	// one character match
		if (c != str[1]) continue;													// 2nd character mismatch
		if ((c = GET_TOKEN_BYTE(token[2])) == NUL) { if (str[2] == NUL) return(i);}	// two character match
		if (c != str[2]) continue;													// 3rd character mismatch
		if ((c = GET_TOKEN_BYTE(token[3])) == NUL) { if (str[3] == NUL) return(i);}	// three character match
		if (c != str[3]) continue;													// 4th character mismatch
		if ((c = GET_TOKEN_BYTE(token[4])) == NUL) { if (str[4] == NUL) return(i);}	// four character match
		if (c != str[4]) continue;													// 5th character mismatch
		return (i);																	// five character match
	}
	return (NO_MATCH);
}

/* 
 * nv_get_type() - returns command type as a NV_TYPE enum
 */

uint8_t nv_get_type(nvObj_t *nv)
{
	if (nv->token[0] == NUL) return (NV_TYPE_NULL);
	if (strcmp("gc", nv->token) == 0) return (NV_TYPE_GCODE);
	if (strcmp("sr", nv->token) == 0) return (NV_TYPE_REPORT);
	if (strcmp("qr", nv->token) == 0) return (NV_TYPE_REPORT);
	if (strcmp("msg",nv->token) == 0) return (NV_TYPE_MESSAGE);
	if (strcmp("err",nv->token) == 0) return (NV_TYPE_MESSAGE); 	// errors are reported as messages
	if (strcmp("n",  nv->token) == 0) return (NV_TYPE_LINENUM);
	return (NV_TYPE_CONFIG);
}

/*
 * nv_persist_offsets() - write any changed G54 (et al) offsets back to NVM
 */

stat_t nv_persist_offsets(uint8_t flag)
{
	if (flag == true) {
		nvObj_t cmd;
		for (uint8_t i=1; i<=COORDS; i++) {
			for (uint8_t j=0; j<AXES; j++) {
				sprintf(cmd.token, "g%2d%c", 53+i, ("xyzabc")[j]);
				cmd.index = nv_get_index((const char_t *)"", cmd.token);
				cmd.value = cm.offset[i][j];
				nv_persist(&cmd);				// only writes changed values
			}
		}
	}
	return (STAT_OK);
}

/******************************************************************************
 * cmdObj low-level object and list operations
 * nv_get_cmdObj()		- setup a cmd object by providing the index
 * nv_reset_obj()		- quick clear for a new cmd object
 * nv_reset_list()		- clear entire header, body and footer for a new use
 * nv_copy_string()	- used to write a string to shared string storage and link it
 * nv_add_object()		- write contents of parameter to  first free object in the body
 * nv_add_integer()	- add an integer value to end of cmd body (Note 1)
 * nv_add_float()		- add a floating point value to end of cmd body
 * nv_add_string()		- add a string object to end of cmd body
 * nv_add_conditional_message() - add a message to cmd body if messages are enabled
 *
 *	Note: Functions that return a cmd pointer point to the object that was modified or
 *	a NULL pointer if there was an error.
 *
 *	Note: Adding a really large integer (like a checksum value) may lose precision due
 *	to the cast to a float. Sometimes it's better to load an integer as a string if
 *	all you want to do is display it.
 *
 *	Note: A trick is to cast all string constants for nv_copy_string(), nv_add_object(),
 *	nv_add_string() and nv_add_conditional_message() to (const char_t *). Examples:
 *
 *		nv_add_string((const char_t *)"msg", string);
 *
 *	On the AVR this will save a little static RAM. The "msg" string will occupy flash
 *	as an initializer and be instantiated in stack RAM when the function executes.
 *	On the ARM (however) this will put the string into flash and skip RAM allocation.
 */

void nv_get_nvObj(nvObj_t *nv)
{
	if (nv->index >= nv_index_max()) { return; }	// sanity

	index_t tmp = nv->index;
	nv_reset_nvObj(nv);
	nv->index = tmp;

	strcpy_P(nv->token, cfgArray[nv->index].token); // token field is always terminated
	strcpy_P(nv->group, cfgArray[nv->index].group); // group field is always terminated

	// special processing for system groups and stripping tokens for groups
	if (nv->group[0] != NUL) {
		if (GET_TABLE_BYTE(flags) & F_NOSTRIP) {
			nv->group[0] = NUL;
		} else {
			strcpy(nv->token, &nv->token[strlen(nv->group)]); // strip group from the token
		}
	}
	((fptrCmd)GET_TABLE_WORD(get))(nv);	// populate the value
}
 
nvObj_t *nv_reset_nvObj(nvObj_t *nv)		// clear a single cmdObj structure
{
	nv->valuetype = TYPE_EMPTY;				// selective clear is much faster than calling memset
	nv->index = 0;
	nv->value = 0;
	nv->precision = 0;
	nv->token[0] = NUL;
	nv->group[0] = NUL;
	nv->stringp = NULL;

	if (nv->pv == NULL) { 					// set depth correctly
		nv->depth = 0;
	} else {
		if (nv->pv->valuetype == TYPE_PARENT) { 
			nv->depth = nv->pv->depth + 1;
		} else {
			nv->depth = nv->pv->depth;
		}
	}
	return (nv);							// return pointer to cmd as a convenience to callers
}

nvObj_t *nv_reset_nvObj_list()				// clear the header and response body
{
	nvStr.wp = 0;							// reset the shared string
	nvObj_t *nv = nv_list;					// set up linked list and initialize elements	
	for (uint8_t i=0; i<NV_LIST_LEN; i++, nv++) {
		nv->pv = (nv-1);					// the ends are bogus & corrected later
		nv->nx = (nv+1);
		nv->index = 0;
		nv->depth = 1;						// header and footer are corrected later
		nv->precision = 0;
		nv->valuetype = TYPE_EMPTY;
		nv->token[0] = NUL;
	}
	(--nv)->nx = NULL;
	nv = nv_list;							// setup response header element ('r')
	nv->pv = NULL;
	nv->depth = 0;
	nv->valuetype = TYPE_PARENT;
	strcpy(nv->token, "r");
	return (nv_body);						// this is a convenience for calling routines
}

stat_t nv_copy_string(nvObj_t *nv, const char_t *src)
{
	if ((nvStr.wp + strlen(src)) > NV_SHARED_STRING_LEN) { return (STAT_BUFFER_FULL);}
	char_t *dst = &nvStr.string[nvStr.wp];
	strcpy(dst, src);						// copy string to current head position
											// string has already been tested for overflow, above
	nvStr.wp += strlen(src)+1;				// advance head for next string
	nv->stringp = (char_t (*)[])dst;
	return (STAT_OK);
}

/* UNUSED
stat_t nv_copy_string_P(nvObj_t *nv, const char_t *src_P)
{
	char_t buf[NV_SHARED_STRING_LEN];
	strncpy_P(buf, src_P, NV_SHARED_STRING_LEN);
	return (nv_copy_string(nv, buf));
}
*/

nvObj_t *nv_add_object(const char_t *token)  // add an object to the body using a token
{
	nvObj_t *nv = nv_body;
	for (uint8_t i=0; i<NV_BODY_LEN; i++) {
		if (nv->valuetype != TYPE_EMPTY) {
			if ((nv = nv->nx) == NULL) return(NULL); // not supposed to find a NULL; here for safety
			continue;
		}
		// load the index from the token or die trying
		if ((nv->index = nv_get_index((const char_t *)"",token)) == NO_MATCH) { return (NULL);}
		nv_get_nvObj(nv);				// populate the object from the index
		return (nv);
	}
	return (NULL);
}

nvObj_t *nv_add_integer(const char_t *token, const uint32_t value)// add an integer object to the body
{
	nvObj_t *nv = nv_body;
	for (uint8_t i=0; i<NV_BODY_LEN; i++) {
		if (nv->valuetype != TYPE_EMPTY) {
			if ((nv = nv->nx) == NULL) return(NULL); // not supposed to find a NULL; here for safety
			continue;
		}
		strncpy(nv->token, token, TOKEN_LEN);
		nv->value = (float) value;
		nv->valuetype = TYPE_INTEGER;
		return (nv);
	}
	return (NULL);
}

nvObj_t *nv_add_data(const char_t *token, const uint32_t value)// add an integer object to the body
{
	nvObj_t *nv = nv_body;
	for (uint8_t i=0; i<NV_BODY_LEN; i++) {
		if (nv->valuetype != TYPE_EMPTY) {
			if ((nv = nv->nx) == NULL) return(NULL); // not supposed to find a NULL; here for safety
			continue;
		}
		strcpy(nv->token, token);
		float *v = (float*)&value;
		nv->value = *v;
		nv->valuetype = TYPE_DATA;
		return (nv);
	}
	return (NULL);
}

nvObj_t *nv_add_float(const char_t *token, const float value)	// add a float object to the body
{
	nvObj_t *nv = nv_body;
	for (uint8_t i=0; i<NV_BODY_LEN; i++) {
		if (nv->valuetype != TYPE_EMPTY) {
			if ((nv = nv->nx) == NULL) return(NULL);		// not supposed to find a NULL; here for safety
			continue;
		}
		strncpy(nv->token, token, TOKEN_LEN);
		nv->value = value;
		nv->valuetype = TYPE_FLOAT;
		return (nv);
	}
	return (NULL);
}

// ASSUMES A RAM STRING. If you need to post a FLASH string use pstr2str to convert it to a RAM string
nvObj_t *nv_add_string(const char_t *token, const char_t *string) // add a string object to the body
{
	nvObj_t *nv = nv_body;
	for (uint8_t i=0; i<NV_BODY_LEN; i++) {
		if (nv->valuetype != TYPE_EMPTY) {
			if ((nv = nv->nx) == NULL) return(NULL);		// not supposed to find a NULL; here for safety
			continue;
		}
		strncpy(nv->token, token, TOKEN_LEN);
		if (nv_copy_string(nv, string) != STAT_OK) { return (NULL);}
		nv->index = nv_get_index((const char_t *)"", nv->token);
		nv->valuetype = TYPE_STRING;
		return (nv);
	}
	return (NULL);
}

/*
 * cm_conditional_message() - queue a RAM string as a message in the response (conditionally)
 *
 *	Note: If you need to post a FLASH string use pstr2str to convert it to a RAM string
 */

nvObj_t *nv_add_conditional_message(const char_t *string)	// conditionally add a message object to the body
{
	if ((cfg.comm_mode == JSON_MODE) && (js.echo_json_messages != true)) { return (NULL);}
	return(nv_add_string((const char_t *)"msg", string));
}

/**** nv_print_list() - print nv_array as JSON or text **********************
 *
 * 	Generate and print the JSON and text mode output strings. Use this function 
 *	for all text and JSON output that wants to be in a response header. 
 *	Don't just printf stuff.
 *
 *	Inputs:
 *	  json_flags = JSON_OBJECT_FORMAT - print just the body w/o header or footer
 *	  json_flags = JSON_RESPONSE_FORMAT - print a full "r" object with footer
 *
 *	  text_flags = TEXT_INLINE_PAIRS - print text as name/value pairs on a single line
 *	  text_flags = TEXT_INLINE_VALUES - print text as comma separated values on a single line
 *	  text_flags = TEXT_MULTILINE_FORMATTED - print text one value per line with formatting string
 */

void nv_print_list(stat_t status, uint8_t text_flags, uint8_t json_flags)
{
	if (cfg.comm_mode == JSON_MODE) {
		json_print_list(status, json_flags);
	} else {
		text_print_list(status, text_flags);
	}
}

/****************************************************************************
 ***** Diagnostics **********************************************************
 ****************************************************************************/

void nv_dump_nv(nvObj_t *nv)
{
	printf ("i:%d, d:%d, t:%d, p:%d, v:%f, g:%s, t:%s, s:%s\n",
			 nv->index,
			 nv->depth,
			 nv->valuetype,
			 nv->precision,
			 (double)nv->value,
			 nv->group,
			 nv->token,
			 (char *)nv->stringp);
}

/****************************************************************************
 ***** Config Unit Tests ****************************************************
 ****************************************************************************/

#ifdef __UNIT_TESTS
#ifdef __UNIT_TEST_CONFIG

#define NVMwr(i,v) { cmd.index=i; cmd.value=v; nv_write_NVM_value(&cmd);}
#define NVMrd(i)   { cmd.index=i; nv_read_NVM_value(&cmd); printf("%f\n", (char *)cmd.value);}

void cfg_unit_tests()
{

// NVM tests
/*	nvObj_t cmd;
	NVMwr(0, 329.01)
	NVMwr(1, 111.01)
	NVMwr(2, 222.02)
	NVMwr(3, 333.03)
	NVMwr(4, 444.04)
	NVMwr(10, 10.10)
	NVMwr(100, 100.100)
	NVMwr(479, 479.479)

	NVMrd(0)
	NVMrd(1)
	NVMrd(2)
	NVMrd(3)
	NVMrd(4)
	NVMrd(10)
	NVMrd(100)
	NVMrd(479)
*/

// config table tests

	index_t i;
//	float val;

//	print_configs("$", NUL);					// no filter (show all)
//	print_configs("$", 'g');					// filter for general parameters
//	print_configs("$", '1');					// filter for motor 1
//	print_configs("$", 'x');					// filter for x axis

	i = nv_get_index((const char_t *)"fb");
	i = nv_get_index((const char_t *)"xfr");
	i = nv_get_index((const char_t *)"g54");

//	i = get_pos_axis(55);
//	i = get_pos_axis(73);
//	i = get_pos_axis(93);
//	i = get_pos_axis(113);

/*
	for (i=0; i<NV_MAX_INDEX; i++) {

		nv_get(&cmd);

		cmd.value = 42;
		nv_set(&cmd);

		val = get_flt_value(i);
		nv_get_token(i, cmd.token);

//		get_friendly(i, string);
		get_format(i, cmd.vstring);
		get_axis(i);							// uncomment main function to test
		get_motor(i);
		nv_set(i, &cmd);
		nv_print(i);
	}

	_parse_config_string("$1po 1", &c);			// returns a number
	_parse_config_string("XFR=1200", &c);		// returns a number
	_parse_config_string("YFR 1300", &c);		// returns a number
	_parse_config_string("zfr	1400", &c);		// returns a number
	_parse_config_string("afr", &c);			// returns a null
	_parse_config_string("Bfr   ", &c);			// returns a null
	_parse_config_string("cfr=wordy", &c);		// returns a null

//	i = cfgget_config_index("gc");
//	i = cfgget_config_index("gcode");
//	i = cfgget_config_index("c_axis_mode");
//	i = cfgget_config_index("AINT_NOBODY_HOME");
	i = cfgget_config_index("firmware_version");
*/
}

#endif
#endif

#ifdef __cplusplus
}
#endif // __cplusplus
