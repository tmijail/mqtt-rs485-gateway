/*
 * MQTT 3.1.1 session owner (MQTT-1, MQTT-2, MQTT-3) for the Sparkplug node.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_MQTT_LINK_H_
#define GW_MQTT_LINK_H_

#include "gw_config.h"

/** Start the session thread; it connects as soon as the network is ready. */
int mqtt_link_start(const struct gw_config *cfg);

#endif /* GW_MQTT_LINK_H_ */
