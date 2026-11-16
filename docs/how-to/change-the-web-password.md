# Change the web password

This procedure tells how to change the web password of a gateway that has a web password. The web password is also the Wi-Fi password of the access point. Thus a change of the web password also changes the Wi-Fi password of the access point.

- To set the first web password on site, refer to [Set the first web password on site](set-the-first-web-password.md).
- For the keys `web.username` and `web.password`, refer to [The configuration document](../reference/configuration.md#web).
- For the reasons why the Wi-Fi password of the access point is the web password, refer to [The security of the access point](../explanation/access-point-security.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The procedures and the images come from the firmware in QEMU.

## Before you start

You need:

- The web UI, with the user name and the current web password.
- A new web password: 8 to 63 characters. Use only the letters A to Z and a to z, the digits, the space and the symbols of a keyboard (printable ASCII). The gateway refuses other letters, for example `é` or `ñ`.

Write the new password in your password manager before you save it.

A save restarts the gateway. The gateway sends the NDEATH, and the polling and the Sparkplug session stop for some seconds.

## Change the password in the web UI

1. Open the web UI: `https://<ip>/` or `https://<hostname>.local/`. Log in with the current password.
2. Click the tab **Configuration**.
3. In the card **Web interface**, click the field **Password (8 to 63)**.
4. Delete `********`, and type the new password. The field **Confirm the password** shows.
5. Type the new password again in the field **Confirm the password**.

   ![The card 'Web interface' of the Configuration tab: the user admin, a new password in the field 'Password (8 to 63)', the same password in the field 'Confirm the password', and the port. Below the second field: after the restart, the browser asks for the new password, and the Wi-Fi password of the access point changes to the new password.](../images/web-password-field.png)

6. Click **Validate** in the bar at the bottom of the window. If the bar gives an error, read the error below the field, and correct the value.
7. Click **Save and restart**. The page loads again after 15 s.
8. When the browser asks for the user name and the password, type the user name and the new password.

If the two values are not the same, the web UI does not save. The error shows below the field **Confirm the password**. Type the new password again in the two fields.

![The Configuration tab after 'Save and restart' with two different passwords. Below the field 'Confirm the password', a red text tells that the two passwords are not the same. The bar at the bottom of the window shows '1 error: Web interface · Confirm the password' with the link 'Go to the field'.](../images/web-password-mismatch.png)

To change the user name, use the field **User** in the same card. The user name has 1 to 32 visible ASCII characters, without `:`.

## The result of the change

- The web UI and the REST API accept only the new password. Change the password in each script that uses the REST API.
- The Wi-Fi password of the access point `GW-Setup-XXXXXX` becomes the new password. A phone that keeps the old password cannot connect. On the phone, remove the saved network. Then connect again with the new password.
- The gateway does not keep the password in clear text. An export shows it as `"********"`.

## Change the password with the REST API

`PUT /api/config` replaces the full configuration document. Thus start from the stored document.

1. Get the stored document:

   ```sh
   curl -k -u admin:<current password> "https://<ip>/api/config?download=1" -o gw-config.json
   ```

2. In `gw-config.json`, replace `"********"` of `web.password` with the new password. Do not change the other `"********"` values.
3. Validate the document:

   ```sh
   curl -k -u admin:<current password> -H "X-Requested-With: curl" -X PUT \
        --data-binary @gw-config.json "https://<ip>/api/config?dry_run=1"
   ```

4. Save the document. Use the same command without `?dry_run=1`.
5. Delete `gw-config.json`. It contains the new password in clear text.

For the routes and the responses, refer to [The REST API](../reference/rest-api.md).

## Do not remove the web password by mistake

> **Caution: Do not save an empty Password field.** Keep `web.password` as `"********"` in each document that you import. An empty value or a document without this key removes the web password.

Before a save that removes the web password, the web UI asks for a confirmation. Click **Cancel** to keep the password. The REST API and the shell do not ask.

![The confirmation 'Remove the web password?' after 'Save and restart' with an empty password field: web.password is empty. After the restart, the gateway accepts only the user admin with the commissioning password of the serial console, the Wi-Fi password of the access point becomes the commissioning password, and a push of BOOT opens the claim window. It has the red button 'Remove the password' and Cancel.](../images/web-password-remove.png)

When the web password is removed:

- The gateway accepts only the user `admin` with the commissioning password. Only the serial console shows this password.
- The Wi-Fi password of the access point becomes the commissioning password.
- A push of BOOT opens the claim window again. A person at the panel can then set a web password, and keep your configuration.

To set a web password again, do the procedure above with the commissioning password as the current password. Or do the procedure of [Set the first web password on site](set-the-first-web-password.md).

## If the new password does not open the web UI

1. Make sure that you type the user name and the password correctly. Each letter must have the correct case (A or a).
2. After 5 failed logins in sequence, the gateway refuses your address for 30 s (`429`). Wait 30 s before you try again.
3. If the password still does not open the web UI, refer to [Recover a gateway](recover-the-gateway.md#nobody-knows-the-web-password). With the USB console, you set a new password and keep the configuration. Without the USB console, you must do a factory reset.
