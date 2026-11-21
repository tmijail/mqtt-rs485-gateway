# Add devices of one type

This procedure tells how to keep the tag list of a device model one time, in a device type, and use it for two or more devices. Example: 10 energy meters of the same model, each with 40 tags. A change of one tag is then one change, not 10.

- For a device with its own tags, and for the fields of a tag, refer to [Add a Modbus device and its tags](add-a-modbus-device.md).
- For the keys and the rules, refer to [`device_types`](../reference/configuration.md#device_types).
- For the rules of the parameters, refer to [The parameters of a device type](../reference/configuration.md#the-parameters-of-a-device-type).
- For the Sparkplug template of a type, refer to [Templates](../reference/sparkplug-messages-and-metrics.md#templates).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The images come from the firmware in QEMU.

## Before you start

You need:

- Access to the web UI of the gateway, with the web password.
- The register map of the device model. All the devices of the type use the same areas, addresses and types.
- The unit ID of each device.
- If the type has parameters: the name, the type and the default of each parameter, and the values of each device.

Each device of a type gets a copy of the tags of the type. The copies count in the limit of 256 tags. For example, 4 devices of a type with 60 tags are 240 tags.

## 1. Make the type from a device

Use this procedure when one device of the model already has its tags in the gateway.

1. Click the tab **Configuration**.
2. In the card **Devices and tags**, open the device that has the tags.
3. Click **Make a type from these tags**.
4. In the dialog, type the **Name of the new type**, for example the model of the device: `Meter`. The name has 1 to 39 bytes, with no `/`, `+` or `#`.

   ![The dialog 'Make a device type from the tags of Meter1': the field 'Name of the new type' with Meter, the hint '1 to 39 bytes. No /, + or #. It is also the name of the Sparkplug template.', the list of what occurs, and the buttons Cancel and 'Make the type'.](../images/device-type-make-dialog.png)

5. Click **Make the type**. If the name is already the name of a type, the dialog gives an error below the field. Type a different name.

The new type gets a copy of the tags. The device then uses the type, and has no tags of its own.

## 2. Or make a new type

1. In the card **Device types**, click **+ device type**. The new type opens.
2. Type the **Name** of the type.
3. Add the tags with **+ tag**, as for the tags of a device. Refer to [Add the tags](add-a-modbus-device.md#4-add-the-tags).

   ![The card 'Device types (1)' of the Configuration tab with the type Meter open: its name, the line 'Used by 1 device: Meter1', the heading Parameters with the text 'This type has no parameters.' and the button '+ parameter', and below the heading Tags the table of its tags and the buttons '+ tag' and 'Remove type'. 'Remove type' is disabled, and the text next to it tells to give the devices another type first.](../images/device-types-card.png)

The card **Device types** is closed by default. Its header shows the number of types and their names. Each type is a row that closes and opens. The browser keeps the open or closed state.

## 3. Add the other devices of the type

1. In the card **Devices and tags**, click **+ device** for each device.
2. Type the **Name** and the **Unit ID** of the device.
3. In the field **Device type**, select the type.
4. The dialog asks before the own tags of the new device go away. Click the button with the name of the type, for example **Use the type Meter**.

   If you click **Cancel**, the field goes back to **(none: own tags)**, and the device keeps its tags.

   ![The device Meter1 with 'Meter' in the field Device type. In place of a tag table, a grey box tells that the device uses the tags of the device type Meter, with the link 'Go to the type Meter'. The device has no button '+ tag'.](../images/device-with-type.png)

A device with a type has no tag table and no button **+ tag**. The grey box gives the number of the tags of the type. Click **Go to the type** to edit them. Each device keeps its own **Unit ID**, **Poll interval (ms)** and the other fields of the device.

## 4. Add the parameters of the type

A parameter is a fixed value of a device that the gateway does not read from the bus. Examples are the manufacturer of a meter and the ratio of its current transformers. The gateway sends the parameters to the host in the birth messages. Do this procedure only if you want parameters.

1. In the card **Device types**, open the type.
2. Below the heading **Parameters**, click **+ parameter**. The table gets a new row with a name such as `Param1`.
3. In the column **Parameter**, type the name, for example `CT ratio`. The name has 1 to 39 bytes, with no `/`, `+` or `#`.
4. In the column **Type**, select the type of the value. Refer to the table below.
5. In the column **Default**, type the value of a device that gives no value of its own. For `bool`, set or clear the check box.
6. Do steps 2 to 5 again for each parameter.

   ![The heading Parameters of the device type Meter, with its (?) button and the text 'Fixed values of each device of the type, for example the manufacturer. The gateway sends them in the birth messages.' The table has the columns Parameter, Type and Default and four rows: Manufacturer, string, Acme; CT ratio, int64, 100; Accuracy class, float64, 0.5; and Billing meter, bool, with a clear check box. Each row has a button x. Below are the button '+ parameter' and the counter '4 of 8'.](../images/device-type-parameters.png)

| **Type** | Value |
|---|---|
| `string` | A text of 63 bytes at most |
| `int64` | An integer |
| `float64` | A number, with a point before the fraction: `0.5` |
| `bool` | Yes or no |

A type has a maximum of 8 parameters. The counter next to the button **+ parameter** gives their number, for example `4 of 8`. If you type no default, the default is an empty text or 0. A clear check box gives the default no. To read the explainer of the parameters, click the (?) button after the heading **Parameters**.

## 5. Give the parameter values of a device

Each device uses the defaults of the type. Do this procedure for each device that has a different value.

1. In the card **Devices and tags**, open the device.
2. Find the heading **Parameters** above the grey box of the tags. It has one field for each parameter of the type.
3. Type the value of the device in the field of the parameter. The grey text in an empty field is the default of the type.
4. For a `bool` parameter, select **Yes** or **No** in the list.
5. To use the default of the type again, erase the text of the field. For a `bool` parameter, select **Default (no)** or **Default (yes)**.

   ![The device Meter1 with the device type Meter. Above the grey box of the tags, the heading Parameters has one field for each parameter of the type. 'CT ratio' has the value 200 of the device. Manufacturer and 'Accuracy class' are empty and show the defaults Acme and 0.5 in grey. 'Billing meter' is a list that shows 'Default (no)'.](../images/device-parameters.png)

A device shows the heading **Parameters** only when its type has parameters. An empty field always uses the default. To give an empty text in place of a default that is not empty, use the JSON tab.

## 6. Validate and save

1. Click **Validate** in the bar at the bottom of the window.
2. If the bar gives an error, click **Go to the field**. An error in a tag of a type opens the card **Device types** and the type, and shows below the field.
3. Correct each parameter value that has an error. For example, `Meter1 · CT ratio` with "Expected an integer" tells that the field does not contain an integer.

   ![The Configuration tab after Validate with the text '200 A' in the parameter 'CT ratio' of Meter1. Below the field, the error of the gateway shows in red. The bar at the bottom of the window shows '1 error: Meter1 · CT ratio' with the link 'Go to the field', and the buttons Validate and 'Save and restart'.](../images/parameter-error.png)

4. Click **Save and restart**.
5. On the host, make sure that the NBIRTH has the template definition of the type. Make sure also that the DBIRTH of each device has a template instance.
6. If the type has parameters, make sure that each instance has each parameter. Its value is the value of the device, or the default of the type.

## Change or remove a type

- To change a tag for all the devices of the type, change it in the card **Device types**.
- To rename a type, change its **Name**. The devices of the type get the new name.
- To give one device its own tags again, select **(none: own tags)** in its field **Device type**. The device gets a copy of the tags of the type, as its own tags.
- **Remove type** is disabled while devices use the type. Give these devices another type, or **(none: own tags)**, first. The validation also refuses a device that gives a type that does not exist.
- When you select another **Device type** for a device, the parameter values of that device go away.

## Change or remove a parameter

- To rename a parameter, change its name in the column **Parameter**. The values of the devices of the type get the new name.
- To change the type of a parameter, select another **Type**. The default converts when it can, for example the text `2` to the number 2. If it cannot, the default goes away.
- After a change of the **Type**, the values of the devices stay. Click **Validate**, and correct each value that has an error.
- To remove a parameter, click **x** in its row. If no device of the type has its own value, the parameter goes away immediately.
- If a device has its own value, the dialog "Remove the parameter …?" shows each such device with its value. Click **Remove the parameter**. The values go away when you save.
- To keep the parameter, click **Cancel** in the dialog.

![The dialog 'Remove the parameter CT ratio?' with the line 'Type Meter · 1 device'. It tells that Meter1 has its own value for this parameter, 200, that the value goes away when you save, and that the gateway then does not send 'CT ratio' in the birth messages. It has the buttons Cancel and 'Remove the parameter'.](../images/parameter-remove-dialog.png)

## If the host is Ignition

- Use only letters, digits and `_` in the name of a parameter. A name with a hyphen or a space can fail in a `{Name}` reference of a UDT.
- After a change of the parameters of a type, delete the UDT definition in Ignition. MQTT Engine then makes the definition again.

For the reasons and the limits, refer to [Notes for Ignition](../reference/sparkplug-behavior.md#notes-for-ignition).

For UMH Core, a template instance gives no value. With `sparkplug.naming: "umh"`, the gateway sends the tags of a device with a type as plain metrics, and it sends no parameters. Refer to [Connect the gateway to UMH Core](connect-to-umh-core.md).
