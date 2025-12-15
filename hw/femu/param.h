#ifndef __FEMU_PARAM_H
#define __FEMU_PARAM_H

#include <cjson/cJSON.h>

struct FemuCtrl;

void dump_config(struct FemuCtrl *n);
int parse_config(struct FemuCtrl *n);

#define DECODE_PARAM(json, name, type, param, default)                  \
    do {                                                                \
        if (json == NULL) {                                             \
            param = default;                                            \
            break;                                                      \
        }                                                               \
        cJSON *handle = cJSON_GetObjectItemCaseSensitive(json, name);   \
        if (cJSON_HasObjectItem(json, name)) {                          \
            if (!cJSON_Is##type(handle)) {                              \
                femu_err("%s is not "#type"\n", name);                  \
                return -1;                                              \
            } else {                                                    \
                param = cJSON_Get##type##Value(handle);                 \
            }                                                           \
        } else {                                                        \
            param = default;                                            \
        }                                                               \
    } while (0)

#define ENCODE_PARAM(json, name, type, param)                           \
    do {                                                                \
        cJSON_Add##type##ToObject(json, name, param);                   \
    } while (0)

#endif