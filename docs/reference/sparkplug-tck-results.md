# Sparkplug TCK results (Edge Node profile)

This document gives the result of each assertion of the Eclipse Sparkplug TCK 3.0.0, Edge Node profile, for the gateway firmware. The summary, the test environment and the reason for each failure are in `idf/README.md` §4.7. To run the TCK, refer to [Run the Sparkplug TCK against the gateway](../how-to/run-the-sparkplug-tck.md).

## Run

| Item | Value |
|---|---|
| Firmware | `main` at 6621c53, QEMU default build (no PSRAM). `MultipleBrokerTest`, `PrimaryHostTest` and `SessionTerminationTest` again after #186 (21b244d, PSRAM build) |
| TCK | Eclipse Sparkplug TCK 3.0.0 (`Eclipse-Sparkplug-TCK-3.0.0.zip`), from the tag `v3.0.0` |
| Broker | HiveMQ CE 2025.5 with the TCK extension; mosquitto 2.0.18 as the second MQTT server of `MultipleBrokerTest` |
| Java | Eclipse Temurin 21.0.13 |
| Date | 2026-11-11, four runs with the same results; the tables show a run with the two MQTT versions |
| Duration | 262 to 270 s for the 14 tests (7 tests × 2 MQTT versions) |

## Legend

| Value | Meaning |
|---|---|
| PASS | The TCK examined the assertion and found no fault |
| FAIL (TCK bug) | The TCK reported a fault, but the TCK is wrong. The harness proved the cause from the captured messages (`KNOWN_RULES` in `idf/tools/e2e/tck.py`) |
| FAIL (gateway) | The TCK reported a fault of the gateway |
| n/e | NOT EXECUTED: the test did not examine the assertion (for example, the MQTT 5 assertion in the MQTT 3.1.1 run) |

The name of each assertion is its `tck-id` without the prefix `tck-id-`. A name with `Monitor:` is a check of the message monitor of the TCK. The monitor runs during each test, so the same monitor check can be in a number of tests. The TCK does not show the monitor checks that a test did not examine.

## Summary

| Test | MQTT | PASS | FAIL (TCK bug) | FAIL (gateway) | n/e |
|---|---|---|---|---|---|
| SessionEstablishmentTest | 3.1.1 | 78 | 2 | 0 | 1 |
| SessionEstablishmentTest | 5 | 78 | 2 | 0 | 1 |
| SessionTerminationTest | 3.1.1 | 24 | 0 | 0 | 1 |
| SessionTerminationTest | 5 | 24 | 0 | 0 | 1 |
| SendDataTest | 3.1.1 | 41 | 0 | 0 | 3 |
| SendDataTest | 5 | 41 | 0 | 0 | 3 |
| SendComplexDataTest | 3.1.1 | 52 | 0 | 0 | 6 |
| SendComplexDataTest | 5 | 52 | 0 | 0 | 6 |
| ReceiveCommandTest | 3.1.1 | 14 | 0 | 0 | 0 |
| ReceiveCommandTest | 5 | 14 | 0 | 0 | 0 |
| PrimaryHostTest | 3.1.1 | 25 | 0 | 0 | 0 |
| PrimaryHostTest | 5 | 25 | 0 | 0 | 0 |
| MultipleBrokerTest | 3.1.1 | 19 | 0 | 0 | 0 |
| MultipleBrokerTest | 5 | 19 | 0 | 0 | 0 |

## SessionEstablishmentTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `case-sensitivity-metric-names` | PASS | PASS |
| `message-flow-device-birth-publish-dbirth-match-edge-node-topic` | PASS | PASS |
| `message-flow-device-birth-publish-dbirth-payload` | PASS | PASS |
| `message-flow-device-birth-publish-dbirth-qos` | PASS | PASS |
| `message-flow-device-birth-publish-dbirth-retained` | PASS | PASS |
| `message-flow-device-birth-publish-dbirth-topic` | PASS | PASS |
| `message-flow-device-birth-publish-nbirth-wait` | PASS | PASS |
| `message-flow-device-dcmd-subscribe` | PASS | PASS |
| `message-flow-edge-node-birth-publish-connect` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-payload` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-payload-bdSeq` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-payload-seq` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-qos` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-retained` | PASS | PASS |
| `message-flow-edge-node-birth-publish-nbirth-topic` | PASS | PASS |
| `message-flow-edge-node-birth-publish-phid-wait` | PASS | PASS |
| `message-flow-edge-node-birth-publish-will-message` | PASS | PASS |
| `message-flow-edge-node-birth-publish-will-message-payload` | PASS | PASS |
| `message-flow-edge-node-birth-publish-will-message-qos` | PASS | PASS |
| `message-flow-edge-node-birth-publish-will-message-topic` | PASS | PASS |
| `message-flow-edge-node-birth-publish-will-message-will-retained` | PASS | PASS |
| `message-flow-edge-node-ncmd-subscribe` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-datatype` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-name` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-name-aliases` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-value` | PASS | PASS |
| `operational-behavior-data-publish-dbirth-values` | PASS | PASS |
| `operational-behavior-data-publish-nbirth-values` | FAIL (TCK bug) | FAIL (TCK bug) |
| `payloads-alias-birth-requirement` | PASS | PASS |
| `payloads-alias-uniqueness` | PASS | PASS |
| `payloads-dbirth-order` | PASS | PASS |
| `payloads-dbirth-qos` | PASS | PASS |
| `payloads-dbirth-retain` | PASS | PASS |
| `payloads-dbirth-seq` | PASS | PASS |
| `payloads-dbirth-seq-inc` | PASS | PASS |
| `payloads-dbirth-timestamp` | PASS | PASS |
| `payloads-ddata-order` | PASS | PASS |
| `payloads-metric-datatype-req` | PASS | PASS |
| `payloads-name-birth-data-requirement` | PASS | PASS |
| `payloads-nbirth-bdseq` | PASS | PASS |
| `payloads-nbirth-bdseq-repeat` | PASS | PASS |
| `payloads-nbirth-qos` | PASS | PASS |
| `payloads-nbirth-rebirth-req` | PASS | PASS |
| `payloads-nbirth-retain` | PASS | PASS |
| `payloads-nbirth-seq` | PASS | PASS |
| `payloads-nbirth-timestamp` | PASS | PASS |
| `payloads-ndata-order` | PASS | PASS |
| `payloads-ndeath-bdseq` | PASS | PASS |
| `payloads-ndeath-seq` | PASS | PASS |
| `payloads-ndeath-will-message` | PASS | PASS |
| `payloads-ndeath-will-message-qos` | PASS | PASS |
| `payloads-ndeath-will-message-retain` | PASS | PASS |
| `payloads-sequence-num-req-nbirth` | PASS | PASS |
| `principles-birth-certificates-order` | PASS | PASS |
| `principles-persistence-clean-session-311` | PASS | n/e |
| `principles-persistence-clean-session-50` | n/e | PASS |
| `topics-dbirth-metrics` | PASS | PASS |
| `topics-dbirth-mqtt` | PASS | PASS |
| `topics-dbirth-seq` | PASS | PASS |
| `topics-dbirth-timestamp` | PASS | PASS |
| `topics-dbirth-topic` | PASS | PASS |
| `topics-nbirth-bdseq-included` | PASS | PASS |
| `topics-nbirth-bdseq-matching` | PASS | PASS |
| `topics-nbirth-metrics` | FAIL (TCK bug) | FAIL (TCK bug) |
| `topics-nbirth-mqtt` | PASS | PASS |
| `topics-nbirth-rebirth-metric` | PASS | PASS |
| `topics-nbirth-seq-num` | PASS | PASS |
| `topics-nbirth-timestamp` | PASS | PASS |
| `topics-nbirth-topic` | PASS | PASS |
| `topics-ndeath-payload` | PASS | PASS |
| `topics-ndeath-seq` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |

## SessionTerminationTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `message-flow-edge-node-birth-publish-will-message-topic` | PASS | PASS |
| `operational-behavior-device-ddeath` | PASS | PASS |
| `operational-behavior-edge-node-intentional-disconnect-ndeath` | PASS | PASS |
| `operational-behavior-edge-node-intentional-disconnect-packet` | PASS | PASS |
| `payloads-ddeath-seq` | PASS | PASS |
| `payloads-ddeath-seq-inc` | PASS | PASS |
| `payloads-ddeath-seq-number` | PASS | PASS |
| `payloads-ddeath-timestamp` | PASS | PASS |
| `payloads-ndeath-will-message-publisher` | PASS | PASS |
| `payloads-ndeath-will-message-publisher-disconnect-mqtt311` | PASS | n/e |
| `payloads-ndeath-will-message-publisher-disconnect-mqtt50` | n/e | PASS |
| `topics-ddeath-mqtt` | PASS | PASS |
| `topics-ddeath-seq-num` | PASS | PASS |
| `topics-ddeath-topic` | PASS | PASS |
| `topics-ndeath-topic` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |

## SendDataTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `payloads-ddata-qos` | PASS | PASS |
| `payloads-ddata-retain` | PASS | PASS |
| `payloads-ddata-seq` | PASS | PASS |
| `payloads-ddata-timestamp` | PASS | PASS |
| `payloads-metric-timestamp-in-UTC` | PASS | PASS |
| `payloads-ndata-qos` | PASS | PASS |
| `payloads-ndata-retain` | PASS | PASS |
| `payloads-ndata-seq` | PASS | PASS |
| `payloads-ndata-timestamp` | PASS | PASS |
| `payloads-template-definition-members` | PASS | PASS |
| `payloads-template-definition-nbirth` | PASS | PASS |
| `payloads-template-definition-nbirth-only` | PASS | PASS |
| `payloads-template-definition-parameters` | n/e | n/e |
| `payloads-template-definition-parameters-default` | n/e | n/e |
| `payloads-template-definition-ref` | PASS | PASS |
| `payloads-template-instance-members` | PASS | PASS |
| `payloads-template-instance-members-birth` | PASS | PASS |
| `payloads-template-instance-members-data` | PASS | PASS |
| `payloads-template-instance-parameters` | n/e | n/e |
| `payloads-template-instance-ref` | PASS | PASS |
| `topics-ddata-mqtt` | PASS | PASS |
| `topics-ddata-payload` | PASS | PASS |
| `topics-ddata-seq-num` | PASS | PASS |
| `topics-ddata-timestamp` | PASS | PASS |
| `topics-ddata-topic` | PASS | PASS |
| `topics-ndata-mqtt` | PASS | PASS |
| `topics-ndata-payload` | PASS | PASS |
| `topics-ndata-seq-num` | PASS | PASS |
| `topics-ndata-timestamp` | PASS | PASS |
| `topics-ndata-topic` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:operational-behavior-data-publish-nbirth` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-ndata-seq-inc` | PASS | PASS |
| `Monitor:payloads-sequence-num-incrementing` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |
| `Monitor:topics-nbirth-metric-reqs` | PASS | PASS |

## SendComplexDataTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `payloads-alias-data-cmd-requirement` | PASS | PASS |
| `payloads-dataset-column-num-headers` | PASS | PASS |
| `payloads-dataset-column-size` | PASS | PASS |
| `payloads-dataset-parameter-type-req` | PASS | PASS |
| `payloads-dataset-types-def` | PASS | PASS |
| `payloads-dataset-types-num` | PASS | PASS |
| `payloads-dataset-types-type` | PASS | PASS |
| `payloads-dataset-types-value` | PASS | PASS |
| `payloads-metric-datatype-not-req` | PASS | PASS |
| `payloads-metric-datatype-value` | PASS | PASS |
| `payloads-metric-datatype-value-type` | PASS | PASS |
| `payloads-metric-propertyvalue-type-req` | PASS | PASS |
| `payloads-metric-propertyvalue-type-type` | PASS | PASS |
| `payloads-metric-propertyvalue-type-value` | PASS | PASS |
| `payloads-name-birth-data-requirement` | PASS | PASS |
| `payloads-name-requirement` | PASS | PASS |
| `payloads-propertyset-keys-array-size` | PASS | PASS |
| `payloads-propertyset-quality-value-type` | PASS | PASS |
| `payloads-propertyset-quality-value-value` | PASS | PASS |
| `payloads-propertyset-values-array-size` | PASS | PASS |
| `payloads-sequence-num-always-included` | PASS | PASS |
| `payloads-template-dataset-value` | PASS | PASS |
| `payloads-template-definition-is-definition` | PASS | PASS |
| `payloads-template-definition-ref` | PASS | PASS |
| `payloads-template-instance-is-definition` | PASS | PASS |
| `payloads-template-instance-ref` | PASS | PASS |
| `payloads-template-is-definition` | PASS | PASS |
| `payloads-template-is-definition-definition` | PASS | PASS |
| `payloads-template-is-definition-instance` | PASS | PASS |
| `payloads-template-parameter-name-required` | n/e | n/e |
| `payloads-template-parameter-name-type` | n/e | n/e |
| `payloads-template-parameter-type-req` | n/e | n/e |
| `payloads-template-parameter-type-value` | n/e | n/e |
| `payloads-template-parameter-value` | n/e | n/e |
| `payloads-template-parameter-value-type` | n/e | n/e |
| `payloads-template-ref-definition` | PASS | PASS |
| `payloads-template-ref-instance` | PASS | PASS |
| `payloads-template-version` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:operational-behavior-data-publish-dbirth` | PASS | PASS |
| `Monitor:operational-behavior-data-publish-dbirth-change` | PASS | PASS |
| `Monitor:operational-behavior-data-publish-nbirth` | PASS | PASS |
| `Monitor:operational-behavior-data-publish-nbirth-change` | PASS | PASS |
| `Monitor:payloads-ddata-seq-inc` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-ndata-seq-inc` | PASS | PASS |
| `Monitor:payloads-sequence-num-incrementing` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:principles-rbe-recommended` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |
| `Monitor:topics-dbirth-metric-reqs` | PASS | PASS |
| `Monitor:topics-nbirth-metric-reqs` | PASS | PASS |

## ReceiveCommandTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `operational-behavior-data-commands-rebirth-action-1` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-action-2` | PASS | PASS |
| `operational-behavior-data-commands-rebirth-action-3` | PASS | PASS |
| `payloads-ndeath-will-message` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |

## PrimaryHostTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `message-flow-edge-node-birth-publish-phid-offline` | PASS | PASS |
| `message-flow-edge-node-birth-publish-phid-wait` | PASS | PASS |
| `message-flow-edge-node-birth-publish-phid-wait-id` | PASS | PASS |
| `message-flow-edge-node-birth-publish-phid-wait-online` | PASS | PASS |
| `message-flow-edge-node-birth-publish-phid-wait-timestamp` | PASS | PASS |
| `operational-behavior-edge-node-termination-host-offline` | PASS | PASS |
| `operational-behavior-edge-node-termination-host-offline-reconnect` | PASS | PASS |
| `operational-behavior-edge-node-termination-host-offline-timestamp` | PASS | PASS |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:host-topic-phid-birth-payload` | PASS | PASS |
| `Monitor:host-topic-phid-death-payload-timestamp-connect` | PASS | PASS |
| `Monitor:message-flow-edge-node-birth-publish-will-message-payload-bdSeq` | PASS | PASS |
| `Monitor:operational-behavior-host-application-connect-will-payload` | PASS | PASS |
| `Monitor:operational-behavior-host-application-host-id` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-state-will-message-payload` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |
| `Monitor:topics-nbirth-bdseq-increment` | PASS | PASS |

## MultipleBrokerTest

| Assertion (`tck-id-` + name) | MQTT 3.1.1 | MQTT 5 |
|---|---|---|
| `operational-behavior-edge-node-birth-sequence-wait` | PASS | PASS |
| `operational-behavior-primary-application-state-with-multiple-servers-single-server` | PASS | PASS |
| `operational-behavior-primary-application-state-with-multiple-servers-state-subs` | PASS | PASS |
| `operational-behavior-primary-application-state-with-multiple-servers-walk` | PASS (FAIL before #186) | PASS (FAIL before #186) |
| `Monitor:case-sensitivity-sparkplug-ids` | PASS | PASS |
| `Monitor:host-topic-phid-birth-payload` | PASS | PASS |
| `Monitor:host-topic-phid-death-payload-timestamp-connect` | PASS | PASS |
| `Monitor:operational-behavior-host-application-connect-will-payload` | PASS | PASS |
| `Monitor:operational-behavior-host-application-host-id` | PASS | PASS |
| `Monitor:payloads-nbirth-seq` | PASS | PASS |
| `Monitor:payloads-state-will-message-payload` | PASS | PASS |
| `Monitor:payloads-timestamp-in-UTC` | PASS | PASS |
| `Monitor:topic-structure` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-device-id-non-associated-message-types` | PASS | PASS |
| `Monitor:topic-structure-namespace-duplicate-device-id-across-edge-node` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-device-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-edge-node-id` | PASS | PASS |
| `Monitor:topic-structure-namespace-valid-group-id` | PASS | PASS |
