# Write a value to a Modbus device or a digital output

This procedure tells how a Sparkplug host writes a value to a Modbus device (DCMD) or to a digital output of the board (NCMD). The gateway writes the value on the bus, reads it again, and publishes the confirmed value.

- For the commands and the node metrics, refer to [Sparkplug messages and metrics](../reference/sparkplug-messages-and-metrics.md#commands).
- For the limits of the commands on a slow broker, refer to [`idf/README.md` §6.5](../../idf/README.md#65-slow-broker-and-command-limits).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The RS485 hardware mode and the digital outputs are compile-only. The E2E tests the commands in QEMU, with a Modbus slave and a model of the outputs.

## Before you start

- The tag must have `"writable": true` in the configuration. Only `coil` and `holding` tags can be writable. Refer to [Add a Modbus device and its tags](add-a-modbus-device.md), step 4.
- The device must be online: the host received its DBIRTH.
- The host must use the Sparkplug datatype of the DBIRTH. For example, an `int16` tag with `"scale": 0.1` is a Float in Sparkplug, not an Int16.

## The rules of a command

| Rule | Result if the command does not obey it |
|---|---|
| The metric has the name or the alias of the DBIRTH | The gateway discards the command |
| The metric is writable | The gateway discards the command |
| The datatype is the datatype of the DBIRTH. A metric without a datatype uses the datatype of the DBIRTH | The gateway discards the command |
| An integer or Boolean tag gets a value without a fraction | The gateway discards the command |
| The value fits in the register after the inverse scale: `raw = (value − offset) / scale` | The gateway discards the command |

A command applies all its metrics or none. The log gives the reason of each discard. A write goes before the polls in the queue of the bus.

After the write, the gateway reads the register again, and publishes the value of the device in a DDATA. If this read fails, the gateway publishes the value that the device acknowledged.

## Write a value from a Sparkplug host

1. In the host, find the device `<device_id>` under the edge node `<group_id>/<edge_node_id>`.
2. Select the writable metric, for example `Setpoint`.
3. Write the new value.
4. Make sure that the host shows the new value, from the DDATA of the gateway.

The host publishes a DCMD on `spBv1.0/<group_id>/DCMD/<edge_node_id>/<device_id>`. Ignition (MQTT Engine), UMH Core and other Sparkplug hosts can send it. UMH Core needs a write flow: refer to [Connect the gateway to UMH Core](connect-to-umh-core.md).

## Write a value from a script

This example uses Python, `paho-mqtt` and the Sparkplug protobuf of the repository. It sets `Setpoint` (an `int16` holding register with `"scale": 0.1`, thus a Float) to 12.5, and the coil `Run` to true.

1. Install the packages:

   ```sh
   pip install paho-mqtt protobuf grpcio-tools
   ```

2. Save this script as `dcmd.py`. Set the broker and the names of your gateway:

   ```python
   import sys
   import time

   import paho.mqtt.client as mqtt

   sys.path.insert(0, "idf/tools")       # the repository, for spb_decode.py
   import spb_decode

   BROKER, PORT = "192.168.1.10", 1883
   TOPIC = "spBv1.0/Plant1/DCMD/GW01/Meter1"   # group, edge node, device

   spb = spb_decode.load_module()          # the Eclipse Tahu sparkplug_b.proto
   p = spb.Payload(timestamp=int(time.time() * 1000))
   m = p.metrics.add(name="Setpoint", datatype=9)   # 9 = Float
   m.float_value = 12.5
   m = p.metrics.add(name="Run", datatype=11)       # 11 = Boolean
   m.boolean_value = True

   c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="dcmd-script")
   c.connect(BROKER, PORT)
   c.loop_start()
   c.publish(TOPIC, p.SerializeToString(), qos=0).wait_for_publish()
   c.loop_stop()
   c.disconnect()
   ```

3. Run the script from the root folder of the repository:

   ```sh
   python dcmd.py
   ```

4. Make sure that the web UI shows the new values: tab **Status**, card **Modbus devices**.

For the datatype of each tag, refer to [The datatypes in Sparkplug](../reference/configuration.md#the-datatypes-in-sparkplug). Use `int_value` for Int8 to UInt32, `long_value` for Int64 and UInt64, `double_value` for Double and `string_value` for String.

## Set a digital output

The gateway publishes the outputs as the node metrics `DO/1` to `DO/8` (Boolean). A host sets an output with an NCMD on `spBv1.0/<group_id>/NCMD/<edge_node_id>`, with the metric `DO/<n>` and a Boolean value.

- An output that a local rule controls does not accept the NCMD. Refer to [Local rules](../reference/configuration.md#local-rules).
- Without a Sparkplug session for `local_io.failsafe_s`, the outputs go to their safe state. Refer to [The safe state of the DOs](../reference/configuration.md#the-safe-state-of-the-dos).

## Other commands of the node

| NCMD metric | Datatype | What it does |
|---|---|---|
| `Node Control/Rebirth` | Boolean `true` | Sends the NBIRTH and all the DBIRTHs again. The web UI button **Rebirth** does the same |
| `Node Control/Reboot` | Boolean `true` | Orderly restart |
| `Node Control/Scan Rate` | UInt32, ms | Sets one poll interval for all devices (50 to 3 600 000). 0 = each device uses its own poll interval |
| `Node Control/Firmware Update` | String, an `https://` URL | Only with `ota.mqtt: true`. Refer to [Update the firmware](update-the-firmware.md) |
| `Node Control/Firmware SHA-256` | String, 64 hexadecimal characters | Only with `ota.mqtt: true`. The SHA-256 of the image for the next update. Refer to [Update the firmware](update-the-firmware.md) |
| `Node Control/Boot Other Slot` | Boolean `true` | Refer to [Update the firmware](update-the-firmware.md) |

## If the value does not change

1. Read the log of the gateway (console or syslog). Find the reason of the discard.
2. Make sure that the datatype of the command is the datatype of the DBIRTH.
3. Make sure that the tag is writable, and that the device is online.
4. Do a test read of the register. Refer to [Add a Modbus device and its tags](add-a-modbus-device.md), step 2.
5. If the device answers with a Modbus exception, read the data sheet of the device. Some registers are writable only in a special mode of the device.
