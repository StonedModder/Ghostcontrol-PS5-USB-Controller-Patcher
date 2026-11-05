/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "gc_setup.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

/* Called with the feature mutex AND current draft policy/source lease held.
 * Project the actual in-memory draft through the production serializer: never
 * read a saved PROFILE_GET or infer capture/guide completion from these rows.
 * Coordinate expressions are retained exactly, including calibration/polarity.
 * All state here is local; this helper cannot publish or change a profile. */
static int gc_setup_preview_add(struct json_object *out,const GcSetup *setup,
                                int visible,const char *destination)
{
    char text[GC_PROFILE_TEXT_MAX];
    struct json_object *rows=json_object_new_array();
    if(!rows) return -1;
    if(visible) {
        int n=gc_profile_serialize(&setup->profile,text,sizeof text);
        if(n<0 || (size_t)n>=sizeof text) {json_object_put(rows);return -1;}
        for(char *line=text;line && *line;) {
            char *next=strchr(line,'\n');
            if(next) *next++=0;
            char *value=strchr(line,'=');
            if(value) {
                *value++=0;
                const char *mapping_source=NULL,*mapping_target=NULL;
                char physical[768],axis[64];
                if(!strncmp(line,"map.",4)) {mapping_source=line+4;mapping_target=value;}
                else if(!strncmp(line,"pattern.",8)) {mapping_source=value;mapping_target=line+8;}
                else if(!strncmp(line,"hid.button.",11)) {
                    int used=snprintf(physical,sizeof physical,"HID %s",value);
                    if(used<0 || (size_t)used>=sizeof physical) {json_object_put(rows);return -1;}
                    mapping_source=physical;mapping_target=line+11;
                } else if(!strncmp(line,"axis.",5) || !strncmp(line,"hid.axis.",9)) {
                    int hid=!strncmp(line,"hid.axis.",9);
                    int used=snprintf(axis,sizeof axis,"AXIS_%s",line+(hid?9:5));
                    if(used<0 || (size_t)used>=sizeof axis) {json_object_put(rows);return -1;}
                    used=snprintf(physical,sizeof physical,"%s%s",hid?"HID ":"",value);
                    if(used<0 || (size_t)used>=sizeof physical) {json_object_put(rows);return -1;}
                    mapping_source=physical;mapping_target=axis;
                }
                if(mapping_source && mapping_target) {
                    struct json_object *row=json_object_new_object();
                    struct json_object *from=json_object_new_string(mapping_source),*to=json_object_new_string(mapping_target);
                    if(!row || !from || !to) {
                        json_object_put(row);json_object_put(from);json_object_put(to);json_object_put(rows);return -1;
                    }
                    json_object_object_add(row,"source",from);
                    json_object_object_add(row,"target",to);
                    if(json_object_array_add(rows,row)) {json_object_put(row);json_object_put(rows);return -1;}
                }
            }
            line=next;
        }
    }
    struct json_object *path=json_object_new_string(visible?setup->path:"");
    struct json_object *profile=json_object_new_string(visible?destination:"");
    if(!path || !profile) {json_object_put(path);json_object_put(profile);json_object_put(rows);return -1;}
    json_object_object_add(out,"path",path);
    json_object_object_add(out,"profile_id",profile);
    json_object_object_add(out,"mappings",rows);
    return 0;
}
