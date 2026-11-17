# Connect the gateway to a Wi-Fi network

This procedure tells how to connect the gateway to a Wi-Fi network, as a backup of Ethernet or as the primary interface.

- For the keys of `network.wifi` and the choice of the interface, refer to [The configuration document](../reference/configuration.md#networkwifi).
- For the route of the list of the networks, refer to [The REST API](../reference/rest-api.md#get-apiwifiscan).

> **Warning: Do not use this document as verified data for the board.** The firmware has never run on the board (#1, #18). The Wi-Fi radio is compile-only. In QEMU, the list of the networks is simulated.

## Before you start

You need:

- The web UI, with the web password.
- A Wi-Fi network on 2.4 GHz. The gateway does not use 5 GHz.
- An open network, or a network with a password (WPA2 or WPA3 personal). The gateway cannot join an enterprise network (802.1X, a user name and a password for each person).
- The password of the network: 8 to 63 characters, or the 64 hexadecimal digits of the PSK.

A save restarts the gateway. The gateway sends the NDEATH, and the polling and the Sparkplug session stop for some seconds.

## Select the network

1. Open the web UI. Click the tab **Configuration**.
2. In the card **Wi-Fi**, click **Find networks**. The gateway looks for networks for approximately 3 s.

   ![The card 'Wi-Fi' of the Configuration tab with the list of the networks open: Plant-IoT, Plant-Office, Plant-Corp (WPA2-Enterprise, not supported), Warehouse-AP2, Guest and DIRECT-7F-HP M479, strongest first, each with its signal in dBm and its security. Plant-IoT is in the field SSID.](../images/wifi-networks.png)

3. In the list, click the network. Its name goes into the field **SSID**.

   If the network is not in the list, type its name in the field **SSID**. A hidden network is never in the list.

4. In the field **Password**, type the password of the network. For an open network, keep the field empty.
5. In **Role**, select the role of the Wi-Fi interface:
   - `backup`: Ethernet carries the MQTT session. The gateway uses Wi-Fi only when Ethernet is down.
   - `primary`: Wi-Fi carries the MQTT session, and Ethernet is the backup.
6. If the network has no DHCP server, clear **DHCP** below **IPv4 of the Wi-Fi interface**. Then type the IP, the netmask and the gateway.
7. Select **Enabled**.

## Save the configuration

1. Click **Validate** in the bar at the bottom of the window. If the bar gives an error, read the error below the field, and correct the value.
2. Click **Save and restart**. The page loads again after 15 s.

## Make sure that the gateway uses the network

1. Click the tab **Status**.
2. In the card **Network and MQTT**, find the row `wifi`.
3. Make sure that the state is `up`, with an IPv4 address.

With the role `backup`, the row `wifi` is `up` but not `(active)` while Ethernet is up.

If the state is `no link`, the gateway did not join the network:

- Make sure that the SSID and the password are correct.
- Make sure that the network uses 2.4 GHz, and that the signal is better than approximately -80 dBm.
- Read the log of the gateway. It gives the reason of each disconnection.
