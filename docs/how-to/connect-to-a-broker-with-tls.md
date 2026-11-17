# Connect to a broker with TLS

This procedure tells how to encrypt the MQTT connection with TLS, with the CA of the broker and, if the broker asks for it, a client certificate (mutual TLS).

- For the keys of `mqtt.tls`, refer to [The configuration document](../reference/configuration.md#mqtttls).
- For the routes of the files, refer to [The REST API](../reference/rest-api.md#the-pem-files).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E examines TLS and the load of a CA file in QEMU.

## Before you start

You need:

- The web UI, with the web password.
- The TLS port of the broker, usually 8883.
- The CA certificate that signed the certificate of the broker, as a PEM file. A broker with a certificate of a public CA needs no file: the gateway has the public root CAs of ESP-IDF.
- For mutual TLS: the certificate of the gateway and its private key, as PEM files. The key must not be encrypted (no password).
- A valid clock on the gateway. The gateway examines the dates of the certificates. Refer to [The clock and the RTC battery](../reference/clock-and-rtc-battery.md).

Each file has a maximum of 16 KiB. Its name has 1 to 32 characters `A-Z a-z 0-9 . _ -`, and must not end with `.json`. The gateway keeps the name of the file on your computer.

A save restarts the gateway. The gateway sends the NDEATH, and the polling and the Sparkplug session stop for some seconds.

## Load the files

1. Open the web UI. Click the tab **Configuration**.
2. In the card **MQTT**, go to the section **TLS**.
3. Below the field **CA file**, click **Load a file…**. Select the CA file on your computer.
4. Read the message below the section. The field **CA file** now shows the file, with the subject and the last day of the certificate below it.
5. For mutual TLS, do steps 3 and 4 again with **Load a file…** below the field **Client certificate**, for the certificate of the gateway. Then do them again below the field **Client key**, for its key.

The field of the button selects the loaded file. If the file is not of the kind of the field, the message tells you, and the field does not change. For example, a private key does not go into **CA file**.

If the message says that the file is already in the gateway, give the file another name on your computer. Then load it again.

## Set the connection

1. In the section **TLS**, select **Encrypt (TLS)**.
2. In the field **Port** of the card **MQTT**, type the TLS port of the broker.
3. Keep the field **Server name** empty, if the certificate of the broker has the name or the IP address of the field **Broker**. If not, type the name that the certificate has.
4. Click **Validate** in the bar at the bottom of the window.

   ![The card 'MQTT' of the Configuration tab: the broker, the port 8883, the protocol, the table of the backup brokers, and the section TLS with Encrypt (TLS) on. The CA file ca.pem shows its subject and its last day, the client certificate gw01.crt its subject, and the client key gw01.key 'Private key, EC P-256'. Each file field has a button 'Load a file…'. Below the section, a message tells that gw01.key is in the gateway.](../images/mqtt-tls.png)

5. If the bar gives an error, read the error below the field:
   - "The key does not agree with the certificate …": the key is not the key of the client certificate. Select the correct key.
   - "… is not in the LittleFS partition": load the file.
6. Click **Save and restart**. The page loads again after 15 s.

## Make sure that the connection operates

1. Click the tab **Status**.
2. In the card **Network and MQTT**, make sure that MQTT is `connected`.

If MQTT stays `disconnected`, read the log of the gateway. It gives the cause of each failed TLS connection, for example a name that the certificate does not have, or an expired certificate.
