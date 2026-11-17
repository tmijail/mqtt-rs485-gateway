# Manage the certificates

This procedure tells how to load the HTTPS certificate of the plant into the gateway, and how to load, replace and remove the PEM files of the MQTT connection and of the firmware updates.

- For the keys that name the PEM files, refer to [The configuration document](../reference/configuration.md#mqtttls).
- For the routes, refer to [The REST API](../reference/rest-api.md#the-pem-files) and [The HTTPS certificate of the web UI](../reference/rest-api.md#the-https-certificate-of-the-web-ui).
- For the first configuration of TLS, refer to [Connect to a broker with TLS](connect-to-a-broker-with-tls.md).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The E2E examines the card **Certificates** and its routes in QEMU.

## Before you start

You need:

- The web UI, with the web password.
- For the HTTPS certificate: the certificate of the plant CA for the gateway (PEM, with its chain), and its private key (PEM, not encrypted). The SANs of the certificate must contain the name or the address with which you open the web UI, for example `gw01.plant.local`.
- For a PEM file: the file on your computer. A CA file, a client certificate and a client key are PEM files.

The buttons of the card **Certificates** change the gateway at once. They do not wait for **Save and restart**.

The gateway keeps 16 PEM files at most. Each PEM file has 16384 bytes at most. The HTTPS certificate with its chain has 3999 bytes at most, and its key also. The two together must also fit in the free space of the NVS: a chain of 3.5 KB with an RSA 2048 key fits.

![The card 'Certificates' of the Configuration tab. The section 'HTTPS certificate of this page' shows the self-signed certificate of the first boot: its source, subject, SANs, last day and SHA-256 fingerprint, with the buttons 'Load a certificate…' and 'Make a new self-signed certificate'. The section 'Files for MQTT TLS and firmware updates' lists ca.pem, gw01.crt and gw01.key with their content, subject, last day and the keys that use them, each with Replace and Remove, and the button 'Load a file…'.](../images/certificates-card.png)

## Load the HTTPS certificate of the plant

At the first boot, the gateway makes a self-signed certificate. The browser gives a warning for it. With a certificate of the plant CA, a computer that trusts the plant CA gives no warning.

1. Open the web UI. Click the tab **Configuration**.
2. In the card **Certificates**, click **Load a certificate…**.
3. Below **Certificate (PEM, with the chain)**, click **Choose a file**. Select the certificate.
4. Below **Private key (PEM, not encrypted)**, click **Choose a file**. Select the key.
5. Read the result of the examination in the dialog:
   - "The gateway examined the files": the dialog shows the subject, the issuer, the SANs and the last day. A yellow line tells you when the SANs do not contain the hostname or the address of the page.
   - "Not loaded: the key does not agree with the certificate": select the key that the plant CA sent with this certificate.

   ![The dialog 'Load the HTTPS certificate of the plant': the certificate gw22-plant.pem with its chain and the key gw22-plant.key are chosen. The gateway examined the files: the subject, the issuer CN=E2E plant CA, a chain of 2 certificates, the SANs, and the key agrees. The list says that the new certificate applies after a restart. It has the button 'Load the certificate' and Cancel.](../images/web-cert-load-dialog.png)

6. Click **Load the certificate**. The card shows "A new certificate waits for the restart".
7. Click **Restart now**. Then click **Restart** in the confirmation.
8. After the restart, open the web UI again. Make sure that the card **Certificates** shows the source "Loaded: plant CA".

To cancel a certificate that waits for the restart, click **Discard**. The gateway then keeps the current certificate.

## Make a new self-signed certificate

Do this procedure after a change of the hostname, when the card shows "The SANs do not contain the hostname". Do it also to remove the certificate of the plant.

1. In the card **Certificates**, click **Make a new self-signed certificate**.
2. Read the confirmation. Click **Make a new certificate**.
3. Click **Restart now**. Then click **Restart** in the confirmation.
4. Open the web UI again. The browser gives a warning for the new certificate. Compare its SHA-256 fingerprint with the fingerprint in the card, or on the console of the gateway, before you accept it.

> **Caution: Load the certificate of the plant again if you need it.** The new self-signed certificate replaces the certificate of the plant and its key at the restart.

## Replace a PEM file

A replaced file keeps its name. The keys of the configuration that name the file do not change. The gateway uses the new content after the next restart.

1. In the card **Certificates**, find the file in the list **Files for MQTT TLS and firmware updates**. The column **Used by** gives the keys that use it.
2. Click **Replace** in the row of the file.
3. Click **Choose a file**. Select the new file.
4. For a client certificate, also select its new key below **Private key (PEM, not encrypted)**. If the new certificate has a new key, the gateway refuses the certificate without it.
5. Read the result of the examination in the dialog.

   ![The dialog 'Replace gw01.crt and gw01.key': the new certificate gw01-2027.crt and the new key gw01-2027.key are chosen. The gateway examined the files: a certificate CN=GW13 and a private key EC P-256 that agrees with it. It has the button 'Replace the files' and Cancel.](../images/pem-replace-dialog.png)

6. Click **Replace the file** (or **Replace the files**).
7. Replace the other files, if necessary. Then restart the gateway one time. Click **Restart** in the card **Restart and reset** of the tab **Status**, or **Save and restart** after a change of the configuration.

The gateway refuses a content of the other kind for a file in use. For example, a private key cannot replace the file of the CA.

## Load a new PEM file

1. In the card **Certificates**, click **Load a file…**.
2. Below **File (PEM)**, click **Choose a file**. Select the file.
3. For a client certificate, also select its key below **Private key (only with a client certificate)**.
4. If necessary, change the name in **Name in the gateway**: 1 to 32 characters `A-Z a-z 0-9 . _ -`.
5. Read the result of the examination. Click **Load the file**.
6. Select the new file in the card **MQTT** or **Firmware updates**. Then click **Save and restart**.

If the dialog says "the file is not PEM", convert the file. For example, `openssl x509 -inform der -in broker-ca.der -out broker-ca.pem` converts a DER certificate.

## Remove a PEM file

You can remove only a file that the saved configuration does not use. The button **Remove** of a file in use is disabled.

1. If the file is in use, select another file in the card **MQTT** or **Firmware updates**. Click **Save and restart**.
2. In the card **Certificates**, click **Remove** in the row of the file.
3. Click **Remove the file** in the confirmation.

The gateway deletes the file from its flash. You cannot get the file back from the gateway.
