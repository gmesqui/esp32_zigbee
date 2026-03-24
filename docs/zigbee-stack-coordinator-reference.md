# Referencia de la red Zigbee en ESP-IDF desde un coordinador

## Alcance

Este documento describe el stack oficial de Zigbee de Espressif desde la perspectiva de la capa de aplicacion de un coordinador Zigbee en `ESP-IDF`, pensado para `ESP32-C5` y otros SoC de Espressif con radio IEEE 802.15.4 (`ESP32-C6`, `ESP32-H2` y similares, segun soporte de componente y build).

Base de referencia usada para este documento:

- `ESP-IDF 5.5.3`
- `espressif/esp-zigbee-lib 1.6.8`
- `espressif/esp-zboss-lib 1.6.4`

Limites intencionados:

- Solo se describe API publica oficial del stack.
- No se describe software especifico del proyecto.
- No se asume ninguna estructura local de firmware.
- Cuando se habla de "dispositivo Zigbee" se propone un modelo de datos recomendado para la aplicacion, porque el stack oficial no expone un `esp_zb_device_t` monolitico para el coordinador.

## Indice

1. [Nombres especiales y su significado](#1-nombres-especiales-y-su-significado)
2. [Mapa mental del stack](#2-mapa-mental-del-stack)
3. [Flujo tipico en un coordinador](#3-flujo-tipico-en-un-coordinador)
4. [Donde entran los mensajes al coordinador](#4-donde-entran-los-mensajes-al-coordinador)
5. [Mensajes que el coordinador puede enviar](#5-mensajes-que-el-coordinador-puede-enviar)
6. [Como modelar un dispositivo Zigbee en la aplicacion](#6-como-modelar-un-dispositivo-zigbee-en-la-aplicacion)
7. [Persistencia del stack, cache y `zb_storage`](#7-persistencia-del-stack-cache-y-zb_storage)
8. [Ejemplos de secuencias de mensajes](#8-ejemplos-de-secuencias-de-mensajes)
9. [Recomendaciones para no saturar la red](#9-recomendaciones-para-no-saturar-la-red)
10. [Criterio practico para una app coordinador](#10-criterio-practico-para-una-app-coordinador)
11. [Fuentes oficiales](#11-fuentes-oficiales)

## 1. Nombres especiales y su significado

Antes de entrar en callbacks y mensajes, conviene fijar las capas, siglas y terminos que se usaran en todo el documento.

- `APS`
  Capa Application Support. Gestiona addressing, endpoints y confirmaciones APS.

- `atributo`
  Es un dato dentro de un cluster. Por ejemplo, temperatura medida, estado on/off o porcentaje de bateria.

- `BDB`
  Base Device Behavior. Conjunto de procedimientos de comisionamiento: formacion de red, steering, Touchlink y Finding & Binding.

- `bind`
  Asociacion entre un cluster origen y un destino para que un dispositivo envie mensajes a otro sin que la aplicacion haga routing manual cada vez.

- `cluster`
  Unidad funcional ZCL. Ejemplos: `On/Off`, `Temperature Measurement`, `IAS Zone`.

- `cluster client`
  Lado que consume un servicio o recibe ciertos comandos.

- `cluster server`
  Lado que expone atributos o implementa comandos del cluster.

- `command direction`
  Direccion logica del comando ZCL:
  - `TO_SRV`
  - `TO_CLI`

- `descriptor`
  Es una estructura ZDO que describe el nodo o un endpoint. Los mas importantes para un coordinador son `node descriptor`, `power descriptor`, `active endpoints` y `simple descriptor`.

- `device_id`
  Identificador del tipo de dispositivo dentro de un perfil, declarado en el simple descriptor del endpoint.

- `endpoint`
  Es un identificador de aplicacion dentro de un nodo. Un mismo dispositivo puede exponer varios endpoints.

- `Extended PAN ID`
  Identificador de 64 bits de la red Zigbee.

- `Finding & Binding`
  Procedimiento BDB para localizar dispositivos compatibles y crear bindings entre ellos de forma asistida.

- `IEEE address`
  Direccion larga de 64 bits, estable y unica por dispositivo.

- `LQI`
  Link Quality Indicator. Estimacion de calidad de enlace.

- `logical channel`
  Canal Zigbee usado por la red. En 2.4 GHz suele estar entre 11 y 26.

- `MAC`
  Es la capa IEEE 802.15.4 por debajo de NWK. Aqui viven canal, tramas MAC y capacidades radio basicas.

- `manufacturer code`
  Codigo del fabricante que aparece, por ejemplo, en node descriptor o en comandos manufacturer-specific.

- `match descriptor`
  Consulta ZDO para encontrar nodos/endpoints que soportan cierto perfil y conjuntos de clusters.

- `NWK`
  Es la capa de red Zigbee. Maneja direccion corta, routing, joins, rejoins y parte de la topologia.

- `NWK address` o `short address`
  Direccion corta de 16 bits asignada por la red. Puede cambiar con el tiempo.

- `node descriptor`
  Descriptor ZDO del nodo completo: capacidades MAC, fabricante, tamanos maximos de transferencia, server mask, etc.

- `PAN ID`
  Identificador corto de red de 16 bits.

- `permit join`
  Estado que determina si la red o un router aceptan nuevos joins.

- `steering` o `network steering`
  Procedimiento BDB por el que un dispositivo intenta encontrar una red Zigbee adecuada y unirse a ella, o bien permite que otros nodos localicen una red abierta y entren en ella.

- `power descriptor`
  Descriptor ZDO con informacion de alimentacion y modo de consumo del nodo.

- `profile ID`
  Perfil de aplicacion. Ejemplos:
  - `0x0000` para ZDO
  - `0x0104` para Home Automation
  - `0x0109` para Smart Energy
  - `0xC05E` para ZLL
  - `0xA1E0` para Green Power

- `reportable change`
  Umbral de cambio usado por `Configure Reporting` para que un atributo analogico decida si debe informar.

- `RSSI`
  Nivel de senal recibido. En callbacks ZCL aparece en `esp_zb_zcl_frame_header_t`.

- `server mask`
  Bitmap del node descriptor que indica roles especiales del nodo, por ejemplo `Trust Center` o `Network Manager`.

- `simple descriptor`
  Descriptor de un endpoint: perfil, device ID, version, lista de input clusters y output clusters.

- `Touchlink`
  Procedimiento BDB de comisionamiento por proximidad entre dispositivos compatibles, tipico en entornos de iluminacion. Permite formar una red o incorporar un nodo usando un intercambio inter-PAN y reglas especificas de proximidad, seguridad y autorizacion.

- `TC` o `Trust Center`
  Autoridad de seguridad de la red Zigbee. En una red clasica, el coordinador centralizado suele ejercer ese papel.

- `TSN`
  Transaction Sequence Number. Numero de secuencia de transaccion ZCL.

- `unbind`
  Eliminacion de una entrada de binding.

- `ZCL`
  Zigbee Cluster Library. Define atributos, comandos y clusters de aplicacion.

- `ZDO`
  Zigbee Device Object. Define discovery, descriptors, management y senales del stack.

- `ZDP`
  Zigbee Device Profile. Perfil usado por ZDO, incluyendo estados y respuestas ZDP.

Con ese vocabulario, el resto del documento se puede leer asi:

- `BDB` prepara o modifica el estado de comisionamiento de la red.
- `ZDO` descubre y describe nodos.
- `ZCL` transporta la semantica de aplicacion.
- `APS` y `NWK` son la infraestructura de transporte y direccionamiento que hay por debajo.

## 2. Mapa mental del stack

Desde un coordinador, la aplicacion no ve "la red Zigbee" como un unico flujo de mensajes. La API oficial separa la informacion en tres planos:

1. `esp_zb_app_signal_handler()`
   Recibe senales globales del stack: arranque, formacion, permit join, autorizacion, leave, update, etc.

2. Callbacks ZDO por peticion
   Cada peticion ZDO que envia la aplicacion (`ieee_addr_req`, `node_desc_req`, `active_ep_req`, `mgmt_lqi_req`, etc.) recibe su respuesta en un callback propio.

3. `esp_zb_core_action_handler_register()`
   Registra un callback comun para acciones y mensajes ZCL: reports, respuestas `Read Attributes`, `Configure Reporting`, comandos IAS, escenas, grupos, termostato, OTA, etc.

La consecuencia practica es importante:

- La aplicacion no trabaja con un "device object" oficial ya armado.
- La aplicacion compone su modelo de dispositivo a partir de:
  - senales BDB/ZDO,
  - respuestas ZDO,
  - mensajes ZCL,
  - y metadatos locales como timestamps, salud y cache.

## 3. Flujo tipico en un coordinador

Un coordinador basado en la API oficial suele seguir este esquema:

1. Inicializa el stack con `esp_zb_init()` y `esp_zb_start()`.
2. Inicia BDB, normalmente con `ESP_ZB_BDB_MODE_NETWORK_FORMATION`.
3. Espera senales de stack en `esp_zb_app_signal_handler()`.
4. Cuando aparece un nodo:
   - recibe `DEVICE_ANNCE`, `DEVICE_UPDATE` o senales relacionadas;
   - resuelve IEEE/NWK si hace falta;
   - interroga descriptores ZDO;
   - descubre endpoints y clusters;
   - lee atributos ZCL;
   - configura reporting si conviene.
5. Mantiene el estado del nodo con reports ZCL, respuestas ZDO y eventos de red.

## 4. Donde entran los mensajes al coordinador

### 4.1 Senales globales del stack (`esp_zb_app_signal_handler`)

Estas son las senales del enum `esp_zb_app_signal_type_t` que un coordinador puede recibir. Algunas son opcionales o dependen de features de build.

#### Arranque y BDB

- `ESP_ZB_ZDO_SIGNAL_DEFAULT_START`
  Arranque en modo no BDB ya completado. Payload: ninguno.

- `ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP`
  El framework del stack esta listo, pero todavia no se ha lanzado BDB. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START`
  Inicializacion basica de dispositivo factory-new terminada, listo para comisionamiento. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT`
  Reentrada o rejoin usando configuracion persistida. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_STEERING`
  Fin de `network steering`. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_FORMATION`
  Fin de `network formation`. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_STEERING_CANCELLED`
  Resultado de cancelar `network steering`. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_FORMATION_CANCELLED`
  Resultado de cancelar `network formation`. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_TC_REJOIN_DONE`
  Fin del procedimiento de `Trust Center rejoin`. Payload: ninguno.

#### Eventos de nodos y topologia

- `ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE`
  Un nodo anuncia que se ha unido o reunido a la red. Payload: `esp_zb_zdo_signal_device_annce_params_t`.
  Campos principales:
  - `device_short_addr`
  - `ieee_addr`
  - `capability`

- `ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED`
  Un nodo ha iniciado asociacion a nivel de red. Payload: `esp_zb_nwk_signal_device_associated_params_t`.

- `ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED`
  El Trust Center informa del resultado de autorizacion. Payload: `esp_zb_zdo_signal_device_authorized_params_t`.
  Campos principales:
  - `long_addr`
  - `short_addr`
  - `authorization_type`
  - `authorization_status`

- `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE`
  Cambio de estado de un nodo visto por TC/padre: join, rejoin, leave, etc. Payload: `esp_zb_zdo_signal_device_update_params_t`.
  Campos principales:
  - `long_addr`
  - `short_addr`
  - `status`
  - `tc_action`
  - `parent_short`

- `ESP_ZB_ZDO_SIGNAL_LEAVE_INDICATION`
  Un hijo abandona la red o se solicita su salida. Payload: `esp_zb_zdo_signal_leave_indication_params_t`.

- `ESP_ZB_ZDO_SIGNAL_LEAVE`
  El propio coordinador ha ejecutado un leave. Payload: `esp_zb_zdo_signal_leave_params_t`.

- `ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS`
  Estado abierto/cerrado de la red. Payload: puntero a `uint8_t`.

- `ESP_ZB_NWK_SIGNAL_NO_ACTIVE_LINKS_LEFT`
  No quedan enlaces/rutas activas. Payload: ninguno.

- `ESP_ZB_NWK_SIGNAL_PANID_CONFLICT_DETECTED`
  Se detecto conflicto de PAN ID. Payload: ninguno.

- `ESP_ZB_NLME_STATUS_INDICATION`
  Fallo o incidencia de red. Payload: `esp_zb_zdo_signal_nwk_status_indication_params_t`.

- `ESP_ZB_ZDO_DEVICE_UNAVAILABLE`
  El stack no ha podido entregar trafico a un nodo. Payload: `esp_zb_zdo_device_unavailable_params_t`.

#### Touchlink y Finding & Binding

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_STARTED`
  Un iniciador Touchlink ha arrancado una red. Payload: `esp_zb_bdb_signal_touchlink_nwk_started_params_t`.

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK_JOINED_ROUTER`
  Un target Touchlink ha entrado como router. Payload: `esp_zb_bdb_signal_touchlink_nwk_joined_router_t`.

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK`
  Resultado global del comisionamiento Touchlink. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK_TARGET`
  El target Touchlink esta en procedimiento. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK_NWK`
  La red Touchlink del target ha sido arrancada. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_TOUCHLINK_TARGET_FINISHED`
  Fin del procedimiento Touchlink del target. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_FINDING_AND_BINDING_TARGET_FINISHED`
  Fin del modo Finding & Binding target. Payload: ninguno.

- `ESP_ZB_BDB_SIGNAL_FINDING_AND_BINDING_INITIATOR_FINISHED`
  Fin del modo Finding & Binding initiator. Payload: ninguno.

#### Produccion, energia y sleep

- `ESP_ZB_COMMON_SIGNAL_CAN_SLEEP`
  El stack considera que el dispositivo puede dormir. En coordinador suele ser menos relevante, pero la senal existe. Payload: `esp_zb_zdo_signal_can_sleep_params_t`.

- `ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY`
  La configuracion de produccion ha sido encontrada o restaurada. Payload: ninguno.

- `ESP_ZB_ZDO_SIGNAL_ERROR`
  Informacion de senal corrupta o incorrecta. Payload: ninguno.

#### Green Power, solo si esta habilitado

- `ESP_ZB_ZGP_SIGNAL_COMMISSIONING`
  Evento de comisionamiento Green Power. Payload: `esp_zb_zgp_signal_commissioning_params_t`.

- `ESP_ZB_ZGP_SIGNAL_MODE_CHANGE`
  Cambio de modo del subsistema GP. Payload: `esp_zb_zgp_signal_mode_change_params_t`.

- `ESP_ZB_ZGP_SIGNAL_APPROVE_COMMISSIONING`
  La app debe aprobar o rechazar un pairing GP. Payload: `esp_zb_zgp_signal_approve_comm_params_t`.
### 4.2 Respuestas ZDO a peticiones de la aplicacion

Estas respuestas tambien son "mensajes recibidos" por el coordinador, pero no llegan por `app_signal`; llegan por el callback asociado a la peticion enviada.

- `esp_zb_zdo_active_scan_request()` -> `esp_zb_zdo_scan_complete_callback_t`
  Devuelve una lista de `esp_zb_network_descriptor_t` con redes descubiertas.

- `esp_zb_zdo_energy_detect_request()` -> `esp_zb_zdo_energy_detect_callback_t`
  Devuelve energia por canal en `esp_zb_energy_detect_channel_info_t`.

- `esp_zb_zdo_device_bind_req()` -> `esp_zb_zdo_bind_callback_t`
  Respuesta ZDO de bind.

- `esp_zb_zdo_device_unbind_req()` -> `esp_zb_zdo_bind_callback_t`
  Respuesta ZDO de unbind.

- `esp_zb_zdo_find_on_off_light()` -> `esp_zb_zdo_match_desc_callback_t`
  Respuesta abreviada para localizar dispositivos compatibles.

- `esp_zb_zdo_find_color_dimmable_light()` -> `esp_zb_zdo_match_desc_callback_t`
  Igual, especializada en luminarias color/dimmable.

- `esp_zb_zdo_match_cluster()` -> `esp_zb_zdo_match_desc_callback_t`
  Respuesta `Match Descriptor` generica: `addr` + `endpoint`.

- `esp_zb_zdo_ieee_addr_req()` -> `esp_zb_zdo_ieee_addr_callback_t`
  Devuelve `esp_zb_zdo_ieee_addr_rsp_t`.
  Campos clave:
  - `ieee_addr`
  - `nwk_addr`
  - `ext_resp` con lista de hijos si el request fue extendido

- `esp_zb_zdo_nwk_addr_req()` -> `esp_zb_zdo_nwk_addr_callback_t`
  Devuelve `esp_zb_zdo_nwk_addr_rsp_t`.
  Campos clave:
  - `ieee_addr`
  - `nwk_addr`
  - `ext_resp`

- `esp_zb_zdo_node_desc_req()` -> `esp_zb_zdo_node_desc_callback_t`
  Devuelve `esp_zb_af_node_desc_t`.
  Campos clave:
  - `node_desc_flags`
  - `mac_capability_flags`
  - `manufacturer_code`
  - `max_buf_size`
  - `max_incoming_transfer_size`
  - `server_mask`
  - `max_outgoing_transfer_size`
  - `desc_capability_field`

- `esp_zb_zdo_power_desc_req()` -> `esp_zb_zdo_power_desc_callback_t`
  Devuelve `esp_zb_zdo_power_desc_rsp_t`.
  Campo estructural clave:
  - `desc.power_desc_flags`

- `esp_zb_zdo_active_ep_req()` -> `esp_zb_zdo_active_ep_callback_t`
  Devuelve cantidad de endpoints y lista de endpoint IDs.

- `esp_zb_zdo_simple_desc_req()` -> `esp_zb_zdo_simple_desc_callback_t`
  Devuelve `esp_zb_af_simple_desc_1_1_t` o layout equivalente generado por ZBOSS.
  Campos conceptuales:
  - `endpoint`
  - `app_profile_id`
  - `app_device_id`
  - `app_device_version`
  - `app_input_cluster_count`
  - `app_output_cluster_count`
  - `app_cluster_list[]`

- `esp_zb_zdo_device_leave_req()` -> `esp_zb_zdo_leave_callback_t`
  Devuelve estado de la orden de leave.

- `esp_zb_zdo_permit_joining_req()` -> `esp_zb_zdo_permit_join_callback_t`
  Devuelve estado de `Permit Joining`.

- `esp_zb_zdo_binding_table_req()` -> `esp_zb_zdo_binding_table_callback_t`
  Devuelve `esp_zb_zdo_binding_table_info_t`.
  Campos clave:
  - `status`
  - `index`
  - `total`
  - `count`
  - `record` con lista de `esp_zb_zdo_binding_table_record_t`

- `esp_zb_zdo_mgmt_lqi_req()` -> `esp_zb_zdo_mgmt_lqi_rsp_callback_t`
  Devuelve `esp_zb_zdo_mgmt_lqi_rsp_t`.
  Campos clave:
  - `neighbor_table_entries`
  - `start_index`
  - `neighbor_table_list_count`
  - `neighbor_table_list[]`

- `esp_zb_zdo_mgmt_nwk_update_req()` -> `esp_zb_zdo_mgmt_nwk_update_notify_callback_t`
  Devuelve `esp_zb_zdo_mgmt_update_notify_t`.
  Campos clave:
  - `status`
  - `scanned_channels`
  - `total_transmission`
  - `transmission_failures`
  - `energy_values[]`

### 4.3 Mensajes ZCL y acciones (`esp_zb_core_action_callback_id_t`)

Aqui esta la otra gran familia de mensajes entrantes. Estos callbacks representan tanto comandos recibidos como respuestas ZCL y eventos de atributos.

#### Estado local de atributos y escenas

- `ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID` -> `esp_zb_zcl_set_attr_value_message_t`
  Escritura local de atributo en un cluster residente en el coordinador.

- `ESP_ZB_CORE_SCENES_STORE_SCENE_CB_ID` -> `esp_zb_zcl_store_scene_message_t`
  Peticion de almacenar una escena.

- `ESP_ZB_CORE_SCENES_RECALL_SCENE_CB_ID` -> `esp_zb_zcl_recall_scene_message_t`
  Peticion de recuperar una escena.

#### IAS, OTA, termostato, metering, door lock, identify, price y commissioning

- `ESP_ZB_CORE_IAS_ZONE_ENROLL_RESPONSE_VALUE_CB_ID` -> `esp_zb_zcl_ias_zone_enroll_response_message_t`
- `ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID` -> `esp_zb_zcl_ota_upgrade_value_message_t`
- `ESP_ZB_CORE_OTA_UPGRADE_SRV_STATUS_CB_ID` -> `esp_zb_zcl_ota_upgrade_server_status_message_t`
- `ESP_ZB_CORE_OTA_UPGRADE_SRV_QUERY_IMAGE_CB_ID` -> `esp_zb_zcl_ota_upgrade_server_query_image_message_t`
- `ESP_ZB_CORE_THERMOSTAT_VALUE_CB_ID` -> `esp_zb_zcl_thermostat_value_message_t`
- `ESP_ZB_CORE_METERING_GET_PROFILE_CB_ID` -> `esp_zb_zcl_metering_get_profile_message_t`
- `ESP_ZB_CORE_METERING_GET_PROFILE_RESP_CB_ID` -> `esp_zb_zcl_metering_get_profile_resp_message_t`
- `ESP_ZB_CORE_METERING_REQ_FAST_POLL_MODE_CB_ID` -> `esp_zb_zcl_metering_request_fast_poll_mode_message_t`
- `ESP_ZB_CORE_METERING_REQ_FAST_POLL_MODE_RESP_CB_ID` -> `esp_zb_zcl_metering_request_fast_poll_mode_resp_message_t`
- `ESP_ZB_CORE_METERING_GET_SNAPSHOT_CB_ID` -> `esp_zb_zcl_metering_get_snapshot_message_t`
- `ESP_ZB_CORE_METERING_PUBLISH_SNAPSHOT_CB_ID` -> `esp_zb_zcl_metering_publish_snapshot_message_t`
- `ESP_ZB_CORE_METERING_GET_SAMPLED_DATA_CB_ID` -> `esp_zb_zcl_metering_get_sampled_data_message_t`
- `ESP_ZB_CORE_METERING_GET_SAMPLED_DATA_RESP_CB_ID` -> `esp_zb_zcl_metering_get_sampled_data_resp_message_t`
- `ESP_ZB_CORE_DOOR_LOCK_LOCK_DOOR_CB_ID` -> `esp_zb_zcl_door_lock_lock_door_message_t`
- `ESP_ZB_CORE_DOOR_LOCK_LOCK_DOOR_RESP_CB_ID` -> `esp_zb_zcl_door_lock_lock_door_resp_message_t`
- `ESP_ZB_CORE_IDENTIFY_EFFECT_CB_ID` -> `esp_zb_zcl_identify_effect_message_t`
- `ESP_ZB_CORE_BASIC_RESET_TO_FACTORY_RESET_CB_ID` -> `esp_zb_zcl_basic_reset_factory_default_message_t`
- `ESP_ZB_CORE_PRICE_GET_CURRENT_PRICE_CB_ID` -> `esp_zb_zcl_price_get_current_price_message_t`
- `ESP_ZB_CORE_PRICE_GET_SCHEDULED_PRICES_CB_ID` -> `esp_zb_zcl_price_get_scheduled_prices_message_t`
- `ESP_ZB_CORE_PRICE_GET_TIER_LABELS_CB_ID` -> `esp_zb_zcl_price_get_tier_labels_message_t`
- `ESP_ZB_CORE_PRICE_PUBLISH_PRICE_CB_ID` -> `esp_zb_zcl_price_publish_price_message_t`
- `ESP_ZB_CORE_PRICE_PUBLISH_TIER_LABELS_CB_ID` -> `esp_zb_zcl_price_publish_tier_labels_message_t`
- `ESP_ZB_CORE_PRICE_PRICE_ACK_CB_ID` -> `esp_zb_zcl_price_ack_message_t`
- `ESP_ZB_CORE_COMM_RESTART_DEVICE_CB_ID` -> `esp_zigbee_zcl_commissioning_restart_device_message_t`
- `ESP_ZB_CORE_COMM_OPERATE_STARTUP_PARAMS_CB_ID` -> `esp_zigbee_zcl_commissioning_operate_startup_parameters_message_t`
- `ESP_ZB_CORE_COMM_COMMAND_RESP_CB_ID` -> `esp_zigbee_zcl_commissioning_command_response_message_t`

#### IAS ACE, IAS WD, Window Covering y DRLC

- `ESP_ZB_CORE_IAS_WD_START_WARNING_CB_ID` -> `esp_zb_zcl_ias_wd_start_warning_message_t`
- `ESP_ZB_CORE_IAS_WD_SQUAWK_CB_ID` -> `esp_zb_zcl_ias_wd_squawk_message_t`
- `ESP_ZB_CORE_IAS_ACE_ARM_CB_ID` -> `esp_zb_zcl_ias_ace_arm_message_t`
- `ESP_ZB_CORE_IAS_ACE_BYPASS_CB_ID` -> `esp_zb_zcl_ias_ace_bypass_message_t`
- `ESP_ZB_CORE_IAS_ACE_EMERGENCY_CB_ID` -> `esp_zb_zcl_ias_ace_emergency_message_t`
- `ESP_ZB_CORE_IAS_ACE_FIRE_CB_ID` -> `esp_zb_zcl_ias_ace_fire_message_t`
- `ESP_ZB_CORE_IAS_ACE_PANIC_CB_ID` -> `esp_zb_zcl_ias_ace_panic_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_PANEL_STATUS_CB_ID` -> `esp_zb_zcl_ias_ace_get_panel_status_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_BYPASSED_ZONE_LIST_CB_ID` -> `esp_zb_zcl_ias_ace_get_bypassed_zone_list_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_ZONE_STATUS_CB_ID` -> `esp_zb_zcl_ias_ace_get_zone_status_message_t`
- `ESP_ZB_CORE_IAS_ACE_ARM_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_arm_response_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_ZONE_ID_MAP_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_get_zone_id_map_response_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_ZONE_INFO_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_get_zone_info_response_message_t`
- `ESP_ZB_CORE_IAS_ACE_ZONE_STATUS_CHANGED_CB_ID` -> `esp_zb_zcl_ias_ace_zone_status_changed_message_t`
- `ESP_ZB_CORE_IAS_ACE_PANEL_STATUS_CHANGED_CB_ID` -> `esp_zb_zcl_ias_ace_panel_status_changed_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_PANEL_STATUS_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_get_panel_status_response_message_t`
- `ESP_ZB_CORE_IAS_ACE_SET_BYPASSED_ZONE_LIST_CB_ID` -> `esp_zb_zcl_ias_ace_set_bypassed_zone_list_message_t`
- `ESP_ZB_CORE_IAS_ACE_BYPASS_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_bypass_response_message_t`
- `ESP_ZB_CORE_IAS_ACE_GET_ZONE_STATUS_RESP_CB_ID` -> `esp_zb_zcl_ias_ace_get_zone_status_response_message_t`
- `ESP_ZB_CORE_WINDOW_COVERING_MOVEMENT_CB_ID` -> `esp_zb_zcl_window_covering_movement_message_t`
- `ESP_ZB_CORE_THERMOSTAT_WEEKLY_SCHEDULE_SET_CB_ID` -> `esp_zb_zcl_thermostat_weekly_schedule_set_message_t`
- `ESP_ZB_CORE_DRLC_LOAD_CONTROL_EVENT_CB_ID` -> `esp_zb_zcl_drlc_load_control_event_message_t`
- `ESP_ZB_CORE_DRLC_CANCEL_LOAD_CONTROL_EVENT_CB_ID` -> `esp_zb_zcl_drlc_cancel_load_control_event_message_t`
- `ESP_ZB_CORE_DRLC_CANCEL_ALL_LOAD_CONTROL_EVENTS_CB_ID` -> `esp_zb_zcl_drlc_cancel_all_load_control_events_message_t`
- `ESP_ZB_CORE_DRLC_REPORT_EVENT_STATUS_CB_ID` -> `esp_zb_zcl_drlc_report_event_status_message_t`
- `ESP_ZB_CORE_DRLC_GET_SCHEDULED_EVENTS_CB_ID` -> `esp_zb_zcl_drlc_get_scheduled_events_message_t`
- `ESP_ZB_CORE_POLL_CONTROL_CHECK_IN_REQ_CB_ID` -> `esp_zb_zcl_poll_control_check_in_req_message_t`

#### Alarms

- `ESP_ZB_CORE_ALARMS_RESET_ALARM_CB_ID` -> `esp_zb_zcl_alarms_reset_alarm_message_t`
- `ESP_ZB_CORE_ALARMS_RESET_ALL_ALARMS_CB_ID` -> `esp_zb_zcl_alarms_reset_all_alarms_message_t`
- `ESP_ZB_CORE_ALARMS_ALARM_CB_ID` -> `esp_zb_zcl_alarms_alarm_message_t`
- `ESP_ZB_CORE_ALARMS_GET_ALARM_RESP_CB_ID` -> `esp_zb_zcl_alarms_get_alarm_resp_message_t`

#### Respuestas ZCL de infraestructura

- `ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID` -> `esp_zb_zcl_cmd_read_attr_resp_message_t`
  Respuesta a `Read Attributes`.

- `ESP_ZB_CORE_CMD_WRITE_ATTR_RESP_CB_ID` -> `esp_zb_zcl_cmd_write_attr_resp_message_t`
  Respuesta a `Write Attributes`.

- `ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID` -> `esp_zb_zcl_cmd_config_report_resp_message_t`
  Respuesta a `Configure Reporting`.

- `ESP_ZB_CORE_CMD_READ_REPORT_CFG_RESP_CB_ID` -> `esp_zb_zcl_cmd_read_report_config_resp_message_t`
  Respuesta a lectura de configuracion de reporting.

- `ESP_ZB_CORE_CMD_DISC_ATTR_RESP_CB_ID` -> `esp_zb_zcl_cmd_discover_attributes_resp_message_t`
  Respuesta a descubrimiento de atributos.

- `ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID` -> `esp_zb_zcl_cmd_default_resp_message_t`
  Respuesta por defecto ZCL.

#### Respuestas de grupos y escenas

- `ESP_ZB_CORE_CMD_OPERATE_GROUP_RESP_CB_ID` -> `esp_zb_zcl_groups_operate_group_resp_message_t`
- `ESP_ZB_CORE_CMD_VIEW_GROUP_RESP_CB_ID` -> `esp_zb_zcl_groups_view_group_resp_message_t`
- `ESP_ZB_CORE_CMD_GET_GROUP_MEMBERSHIP_RESP_CB_ID` -> `esp_zb_zcl_groups_get_group_membership_resp_message_t`
- `ESP_ZB_CORE_CMD_OPERATE_SCENE_RESP_CB_ID` -> `esp_zb_zcl_scenes_operate_scene_resp_message_t`
- `ESP_ZB_CORE_CMD_VIEW_SCENE_RESP_CB_ID` -> `esp_zb_zcl_scenes_view_scene_resp_message_t`
- `ESP_ZB_CORE_CMD_GET_SCENE_MEMBERSHIP_RESP_CB_ID` -> `esp_zb_zcl_scenes_get_scene_membership_resp_message_t`

#### IAS Zone, custom cluster, Touchlink, Green Power y reporting

- `ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_ENROLL_REQUEST_ID` -> `esp_zb_zcl_ias_zone_enroll_request_message_t`
- `ESP_ZB_CORE_CMD_IAS_ZONE_ZONE_STATUS_CHANGE_NOT_ID` -> `esp_zb_zcl_ias_zone_status_change_notification_message_t`
- `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID` -> `esp_zb_zcl_custom_cluster_command_message_t`
- `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_RESP_CB_ID` -> `esp_zb_zcl_custom_cluster_command_message_t`
- `ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID` -> `esp_zb_zcl_privilege_command_message_t`
- `ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_RESP_CB_ID` -> `esp_zb_zcl_privilege_command_message_t`
- `ESP_ZB_CORE_CMD_TOUCHLINK_GET_GROUP_ID_RESP_CB_ID` -> `esp_zb_touchlink_get_group_identifiers_resp_message_t`
- `ESP_ZB_CORE_CMD_TOUCHLINK_GET_ENDPOINT_LIST_RESP_CB_ID` -> `esp_zb_zcl_touchlink_get_endpoint_list_resp_message_t`
- `ESP_ZB_CORE_CMD_THERMOSTAT_GET_WEEKLY_SCHEDULE_RESP_CB_ID` -> `esp_zb_zcl_thermostat_get_weekly_schedule_resp_message_t`
- `ESP_ZB_CORE_CMD_GREEN_POWER_RECV_CB_ID` -> `esp_zb_zcl_cmd_green_power_recv_message_t`
- `ESP_ZB_CORE_REPORT_ATTR_CB_ID` -> `esp_zb_zcl_report_attr_message_t`
## 5. Mensajes que el coordinador puede enviar

### 5.1 Control del stack y BDB

Aunque no todo es "mensaje de aplicacion" en sentido estricto, estas APIs determinan que trafico de red puede generar el coordinador:

- `esp_zb_start()`
  Arranca el stack en modo `autostart` o `no-autostart`.

- `esp_zb_bdb_start_top_level_commissioning(mode_mask)`
  Inicia un procedimiento BDB.
  Modos relevantes:
  - `ESP_ZB_BDB_MODE_INITIALIZATION`
  - `ESP_ZB_BDB_MODE_TOUCHLINK_COMMISSIONING`
  - `ESP_ZB_BDB_MODE_NETWORK_STEERING`
  - `ESP_ZB_BDB_MODE_NETWORK_FORMATION`
  - `ESP_ZB_BDB_MODE_TOUCHLINK_TARGET`

- `esp_zb_bdb_open_network(permit_duration)`
  Abre la red para joins.

- `esp_zb_bdb_close_network()`
  Cierra la red para joins.

- `esp_zb_bdb_reset_via_local_action()`
  Leave local con limpieza de persistencia Zigbee.

- `esp_zb_factory_reset()`
  Borrado completo de `zb_storage` y reinicio.

- `esp_zb_set_primary_network_channel_set(channel_mask)`
  Define canales primarios del stack.

- `esp_zb_bdb_set_scan_duration(duration)`
  Ajusta duracion de scan BDB.

- `esp_zb_set_bdb_commissioning_mode()`
  Fija el modo BDB por defecto del stack.

- `esp_zb_set_tx_power()` y `esp_zb_get_tx_power()`
  Gestion de potencia RF.

- `esp_zb_zdo_touchlink_set_nwk_channel()`
  Ajuste de canal para Touchlink.

- `esp_zb_bdb_finding_binding_start_target()`
- `esp_zb_bdb_finding_binding_cancel_target()`
- `esp_zb_bdb_finding_binding_start_initiator()`
- `esp_zb_bdb_finding_binding_cancel_initiator()`
  Control del flujo Finding & Binding.

### 5.2 Peticiones ZDO que el coordinador puede emitir

Estas son las primitivas ZDO publicas relevantes en la API oficial:

- `esp_zb_zdo_active_scan_request()`
  Descubrimiento de redes.

- `esp_zb_zdo_energy_detect_request()`
  Medida de energia por canal.

- `esp_zb_zdo_device_bind_req()`
  Crear binding.

- `esp_zb_zdo_device_unbind_req()`
  Eliminar binding.

- `esp_zb_zdo_find_on_off_light()`
  Match descriptor especializado.

- `esp_zb_zdo_find_color_dimmable_light()`
  Match descriptor especializado.

- `esp_zb_zdo_match_cluster()`
  Match descriptor generico.

- `esp_zb_zdo_ieee_addr_req()`
  Resolver IEEE a partir de NWK.

- `esp_zb_zdo_nwk_addr_req()`
  Resolver NWK a partir de IEEE.

- `esp_zb_zdo_node_desc_req()`
  Leer node descriptor.

- `esp_zb_zdo_power_desc_req()`
  Leer power descriptor.

- `esp_zb_zdo_simple_desc_req()`
  Leer simple descriptor de un endpoint.

- `esp_zb_zdo_active_ep_req()`
  Obtener lista de endpoints activos.

- `esp_zb_zdo_device_leave_req()`
  Solicitar leave a un dispositivo.

- `esp_zb_zdo_permit_joining_req()`
  Emitir `Mgmt_Permit_Joining_req`.

- `esp_zb_zdo_binding_table_req()`
  Leer binding table remota.

- `esp_zb_zdo_device_announcement_req()`
  Emitir `Device_annce`.

- `esp_zb_zdo_mgmt_lqi_req()`
  Leer neighbor table / LQI remota.

- `esp_zb_zdo_mgmt_nwk_update_req()`
  Escanear energia, solicitar cambio de canal o actualizar parametros NWK.

### 5.3 Comandos ZCL genericos

Estas APIs sirven para casi todos los clusters, incluidos los clusters de sensores que no tienen comandos especificos propios:

- `esp_zb_zcl_read_attr_cmd_req()`
  Leer atributos.

- `esp_zb_zcl_write_attr_cmd_req()`
  Escribir atributos.

- `esp_zb_zcl_report_attr_cmd_req()`
  Enviar un report de atributo.

- `esp_zb_zcl_config_report_cmd_req()`
  Configurar reporting.

- `esp_zb_zcl_read_report_config_cmd_req()`
  Leer configuracion de reporting.

- `esp_zb_zcl_disc_attr_cmd_req()`
  Descubrir atributos de un cluster.

Esto es importante para la capa de aplicacion:

- Muchos clusters del SDK son principalmente orientados a atributos.
- En esos casos, la mayor parte del trafico util del coordinador se hace con:
  - `Read Attributes`,
  - `Write Attributes`,
  - `Configure Reporting`,
  - `Read Reporting Configuration`,
  - `Discover Attributes`,
  - y reports entrantes.

### 5.4 Comandos ZCL especificos por familia

Estas son las familias de comandos ZCL con APIs especificas de envio en `esp_zigbee_zcl_command.h`.

#### Basic, On/Off, Identify, Commissioning

- `esp_zb_zcl_basic_factory_reset_cmd_req()`
- `esp_zb_zcl_on_off_cmd_req()`
- `esp_zb_zcl_on_off_off_with_effect_cmd_req()`
- `esp_zb_zcl_on_off_on_with_recall_global_scene_cmd_req()`
- `esp_zb_zcl_on_off_on_with_timed_off_cmd_req()`
- `esp_zb_zcl_identify_cmd_req()`
- `esp_zb_zcl_identify_trigger_effect_cmd_req()`
- `esp_zb_zcl_identify_query_cmd_req()`
- `esp_zb_zcl_comm_restart_device_cmd_req()`
- `esp_zb_zcl_comm_save_startup_params_cmd_req()`
- `esp_zb_zcl_comm_restore_startup_params_cmd_req()`
- `esp_zb_zcl_comm_reset_startup_params_cmd_req()`

#### Level Control

- `esp_zb_zcl_level_move_to_level_cmd_req()`
- `esp_zb_zcl_level_move_to_level_with_onoff_cmd_req()`
- `esp_zb_zcl_level_move_cmd_req()`
- `esp_zb_zcl_level_move_with_onoff_cmd_req()`
- `esp_zb_zcl_level_step_cmd_req()`
- `esp_zb_zcl_level_step_with_onoff_cmd_req()`
- `esp_zb_zcl_level_stop_cmd_req()`

#### Color Control

- `esp_zb_zcl_color_move_to_hue_cmd_req()`
- `esp_zb_zcl_color_move_hue_cmd_req()`
- `esp_zb_zcl_color_step_hue_cmd_req()`
- `esp_zb_zcl_color_move_to_saturation_cmd_req()`
- `esp_zb_zcl_color_move_saturation_cmd_req()`
- `esp_zb_zcl_color_step_saturation_cmd_req()`
- `esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req()`
- `esp_zb_zcl_color_move_to_color_cmd_req()`
- `esp_zb_zcl_color_move_color_cmd_req()`
- `esp_zb_zcl_color_step_color_cmd_req()`
- `esp_zb_zcl_color_stop_move_step_cmd_req()`
- `esp_zb_zcl_color_move_to_color_temperature_cmd_req()`
- `esp_zb_zcl_color_enhanced_move_to_hue_cmd_req()`
- `esp_zb_zcl_color_enhanced_move_hue_cmd_req()`
- `esp_zb_zcl_color_enhanced_step_hue_cmd_req()`
- `esp_zb_zcl_color_enhanced_move_to_hue_saturation_cmd_req()`
- `esp_zb_zcl_color_color_loop_set_cmd_req()`
- `esp_zb_zcl_color_move_color_temperature_cmd_req()`
- `esp_zb_zcl_color_step_color_temperature_cmd_req()`

#### Door Lock, Groups y Scenes

- `esp_zb_zcl_lock_door_cmd_req()`
- `esp_zb_zcl_unlock_door_cmd_req()`
- `esp_zb_zcl_groups_add_group_cmd_req()`
- `esp_zb_zcl_groups_remove_group_cmd_req()`
- `esp_zb_zcl_groups_remove_all_groups_cmd_req()`
- `esp_zb_zcl_groups_view_group_cmd_req()`
- `esp_zb_zcl_groups_get_group_membership_cmd_req()`
- `esp_zb_zcl_scenes_add_scene_cmd_req()`
- `esp_zb_zcl_scenes_remove_scene_cmd_req()`
- `esp_zb_zcl_scenes_remove_all_scenes_cmd_req()`
- `esp_zb_zcl_scenes_view_scene_cmd_req()`
- `esp_zb_zcl_scenes_store_scene_cmd_req()`
- `esp_zb_zcl_scenes_recall_scene_cmd_req()`
- `esp_zb_zcl_scenes_get_scene_membership_cmd_req()`

#### IAS

- `esp_zb_zcl_ias_zone_status_change_notif_cmd_req()`
- `esp_zb_zcl_ias_zone_enroll_cmd_req()`
- `esp_zb_zcl_ias_ace_arm_cmd_req()`
- `esp_zb_zcl_ias_ace_bypass_cmd_req()`
- `esp_zb_zcl_ias_ace_emergency_cmd_req()`
- `esp_zb_zcl_ias_ace_fire_cmd_req()`
- `esp_zb_zcl_ias_ace_panic_cmd_req()`
- `esp_zb_zcl_ias_ace_get_zone_id_map_cmd_req()`
- `esp_zb_zcl_ias_ace_get_zone_information_cmd_req()`
- `esp_zb_zcl_ias_ace_get_panel_status_cmd_req()`
- `esp_zb_zcl_ias_ace_get_bypassed_zone_list_cmd_req()`
- `esp_zb_zcl_ias_ace_get_zone_status_cmd_req()`
- `esp_zb_zcl_ias_ace_zone_status_changed_cmd_req()`
- `esp_zb_zcl_ias_ace_panel_status_changed_cmd_req()`
- `esp_zb_zcl_ias_wd_start_warning_cmd_req()`
- `esp_zb_zcl_ias_wd_squawk_cmd_req()`
#### Window Covering, Thermostat y Metering

- `esp_zb_zcl_window_covering_cluster_send_cmd_req()`
- `esp_zb_zcl_thermostat_setpoint_raise_lower_cmd_req()`
- `esp_zb_zcl_thermostat_set_weekly_schedule_cmd_req()`
- `esp_zb_zcl_thermostat_get_weekly_schedule_cmd_req()`
- `esp_zb_zcl_thermostat_clear_weekly_schedule_cmd_req()`
- `esp_zb_zcl_thermostat_get_relay_status_log_cmd_req()`
- `esp_zb_zcl_metering_get_profile_cmd_req()`
- `esp_zb_zcl_metering_request_fast_poll_mode_cmd_req()`
- `esp_zb_zcl_metering_get_snapshot_cmd_req()`
- `esp_zb_zcl_metering_get_sampled_data_cmd_req()`

#### Price, Poll Control, Alarms y Custom Cluster

- `esp_zb_zcl_price_get_current_price_cmd_req()`
- `esp_zb_zcl_price_get_scheduled_prices_cmd_req()`
- `esp_zb_zcl_price_get_tier_labels_cmd_req()`
- `esp_zb_zcl_poll_control_check_in_cmd_req()`
- `esp_zb_zcl_poll_control_fast_poll_stop_cmd_req()`
- `esp_zb_zcl_poll_control_set_long_poll_interval_cmd_req()`
- `esp_zb_zcl_poll_control_set_short_poll_interval_cmd_req()`
- `esp_zb_zcl_alarms_reset_alarm_cmd_req()`
- `esp_zb_zcl_alarms_reset_all_alarms_cmd_req()`
- `esp_zb_zcl_alarms_get_alarm_cmd_req()`
- `esp_zb_zcl_alarms_reset_alarm_log_cmd_req()`
- `esp_zb_zcl_alarms_alarm_cmd_req()`
- `esp_zb_zcl_custom_cluster_cmd_req()`

### 5.5 Clusters soportados por el SDK que suelen explotarse via atributos

El SDK expone cabeceras y tipos para muchos clusters que, desde un coordinador, suelen usarse sobre todo mediante los comandos ZCL genericos del apartado anterior. Ejemplos importantes:

- `analog_input`
- `analog_output`
- `analog_value`
- `binary_input`
- `binary_output`
- `binary_value`
- `carbon_dioxide_measurement`
- `device_temperature_configuration`
- `dehumidification_control`
- `diagnostics`
- `electrical_measurement`
- `ec_measurement`
- `fan_control`
- `flow_measurement`
- `humidity_measurement`
- `illuminance_measurement`
- `meter_identification`
- `multistate_input`
- `multistate_output`
- `multistate_value`
- `occupancy_sensing`
- `on_off_switch_configuration`
- `pm2_5_measurement`
- `ph_measurement`
- `power_configuration`
- `pressure_measurement`
- `shade_config`
- `temperature_measurement`
- `thermostat_ui_configuration`
- `time`
- `wind_speed_measurement`

Ademas, si esta habilitado Green Power, existen APIs adicionales en `zgp/`.

## 6. Como modelar un dispositivo Zigbee en la aplicacion

### 6.1 Idea central

La API oficial no impone una estructura unica publica tipo `esp_zb_device_t` para un nodo remoto. Desde la perspectiva de un coordinador, lo correcto es construir un modelo propio a partir de:

- `DEVICE_ANNCE`
- `DEVICE_UPDATE`
- `DEVICE_AUTHORIZED`
- `ieee_addr_rsp`
- `nwk_addr_rsp`
- `node_desc_rsp`
- `power_desc_rsp`
- `active_ep_rsp`
- `simple_desc_rsp`
- `binding_table_rsp`
- `mgmt_lqi_rsp`
- reports ZCL
- respuestas `Read Attributes` y otras respuestas ZCL

### 6.2 Estructura recomendada para la aplicacion

Una forma solida de representar un nodo remoto desde el coordinador es esta:

```c
typedef struct {
    uint8_t endpoint_id;
    uint16_t profile_id;
    uint16_t device_id;
    uint8_t device_version;
    uint8_t input_cluster_count;
    uint16_t *input_clusters;
    uint8_t output_cluster_count;
    uint16_t *output_clusters;
} zigbee_endpoint_record_t;

typedef struct {
    uint8_t endpoint_id;
    uint16_t cluster_id;
    uint16_t attr_id;
    uint8_t attr_type;
    uint16_t value_size;
    void *value_copy;
    bool from_report;
    uint32_t last_update_ms;
} zigbee_attr_cache_t;

typedef struct {
    uint64_t ieee_addr;
    uint16_t nwk_addr;

    bool seen_in_device_annce;
    bool authorized;
    uint8_t authorization_type;
    uint8_t authorization_status;

    uint8_t update_status;
    uint8_t tc_action;
    uint16_t parent_nwk_addr;

    uint16_t node_desc_flags;
    uint8_t mac_capability_flags;
    uint16_t manufacturer_code;
    uint8_t max_buf_size;
    uint16_t max_incoming_transfer_size;
    uint16_t max_outgoing_transfer_size;
    uint16_t server_mask;
    uint8_t desc_capability_field;

    uint16_t power_desc_flags;

    uint8_t endpoint_count;
    zigbee_endpoint_record_t *endpoints;

    uint8_t neighbor_count;
    /* lista opcional basada en Mgmt_Lqi_rsp */

    uint8_t binding_count;
    /* lista opcional basada en BindingTable_rsp */

    uint8_t attr_count;
    zigbee_attr_cache_t *attrs;

    uint8_t last_lqi;
    int8_t last_rssi;
    uint32_t last_seen_ms;
} zigbee_device_record_t;
```

### 6.3 Significado de cada campo

#### Identidad y direccionamiento

- `ieee_addr`
  Identidad global de 64 bits del nodo. Es el campo mas estable.

- `nwk_addr`
  Direccion corta actual de 16 bits. Puede cambiar tras rejoins o reassignments.

#### Presencia y seguridad

- `seen_in_device_annce`
  Marca de que el nodo ha anunciado su presencia con `DEVICE_ANNCE`.

- `authorized`
  Indica si la autorizacion vista por la aplicacion fue satisfactoria.

- `authorization_type`
  Tipo de autorizacion reportado por `DEVICE_AUTHORIZED`.

- `authorization_status`
  Resultado concreto del tipo de autorizacion.

- `update_status`
  Ultimo estado recibido en `DEVICE_UPDATE`, por ejemplo secured rejoin, unsecured join, leave o TC rejoin.

- `tc_action`
  Accion del Trust Center asociada al update.

- `parent_nwk_addr`
  Direccion corta del padre del nodo, util para topologia y diagnostico.

#### Node descriptor

- `node_desc_flags`
  Campo empaquetado del descriptor de nodo. Contiene informacion de tipo logico y capacidades internas del nodo segun la especificacion.

- `mac_capability_flags`
  Capacidades MAC del nodo, por ejemplo si es mains-powered, si su receptor suele quedar activo o si puede aceptar asociaciones.

- `manufacturer_code`
  Codigo numerico del fabricante a nivel Zigbee.

- `max_buf_size`
  Tamano maximo de buffer declarado por el nodo.

- `max_incoming_transfer_size`
  Transferencia maxima entrante declarada.

- `max_outgoing_transfer_size`
  Transferencia maxima saliente declarada.

- `server_mask`
  Bitmap con roles especiales del nodo, por ejemplo `Trust Center`, `Binding Table Center` o `Network Manager`.

- `desc_capability_field`
  Flags sobre soporte de listas extendidas de descriptors.

#### Power descriptor

- `power_desc_flags`
  Campo empaquetado con modo de energia, fuentes de alimentacion disponibles, fuente actual y nivel relativo.

#### Endpoints y clusters

- `endpoint_count`
  Numero de endpoints activos que la aplicacion ha aprendido.

- `endpoints`
  Lista dinamica con un registro por endpoint.

Dentro de `zigbee_endpoint_record_t`:

- `endpoint_id`
  Identificador del endpoint.

- `profile_id`
  Perfil de aplicacion del endpoint.

- `device_id`
  Tipo de dispositivo dentro del perfil.

- `device_version`
  Version del device type declarada por el endpoint.

- `input_cluster_count`
  Numero de clusters servidor en ese endpoint.

- `input_clusters`
  Lista de clusters que el endpoint expone para ser leidos/comandados.

- `output_cluster_count`
  Numero de clusters cliente en ese endpoint.

- `output_clusters`
  Lista de clusters desde los que el endpoint suele originar comandos.

#### Cache de atributos

- `attr_count`
  Numero de atributos cacheados localmente.

- `attrs`
  Lista dinamica de `zigbee_attr_cache_t`.

Dentro de `zigbee_attr_cache_t`:

- `endpoint_id`
  Endpoint al que pertenece el atributo.

- `cluster_id`
  Cluster al que pertenece el atributo.

- `attr_id`
  Identificador del atributo dentro del cluster.

- `attr_type`
  Tipo ZCL del atributo.

- `value_size`
  Tamano del valor copiado localmente.

- `value_copy`
  Copia del valor. Es mejor copiarlo que guardar punteros del callback.

- `from_report`
  Distingue si el valor entro por `Report Attributes` o por lectura explicita.

- `last_update_ms`
  Timestamp local de la ultima actualizacion.

#### Salud y enlace

- `last_lqi`
  Ultimo LQI observado para el nodo.

- `last_rssi`
  Ultimo RSSI observado para trafico ZCL que expuso este metadato.

- `last_seen_ms`
  Timestamp local de la ultima actividad util del nodo.

#### Topologia opcional

- `neighbor_count`
  Numero de vecinos conocidos via `Mgmt_Lqi_rsp`.

- `binding_count`
  Numero de bindings conocidos via `BindingTable_rsp`.

Estas listas son opcionales porque no todos los coordinadores necesitan mantener una copia completa permanente.

### 6.4 Regla de oro

En la aplicacion, la clave primaria real del nodo debe ser `ieee_addr`. El `nwk_addr` debe tratarse como un atributo mutable.

## 7. Persistencia del stack, cache y `zb_storage`

Esta es una distincion importante:

- el stack Zigbee mantiene estado interno propio;
- parte de ese estado puede persistirse entre reinicios;
- y, aparte, la aplicacion puede mantener su propia cache o base de datos de nodos.

Las tres cosas no son lo mismo.

### 7.1 Que es `zb_storage`

`zb_storage` es la particion de persistencia Zigbee del stack. La API oficial la menciona explicitamente en `esp_zb_factory_reset()`: el `factory reset` borra completamente `zb_storage` y reinicia el dispositivo.

Desde la perspectiva de la aplicacion, lo importante es esto:

- si `zb_storage` se conserva, el stack puede arrancar con contexto previo de red;
- si `zb_storage` se borra, el stack vuelve a un estado equivalente a no tener persistencia Zigbee previa;
- la estructura exacta interna de `zb_storage` no debe considerarse parte del contrato publico de alto nivel.

En otras palabras, la aplicacion debe pensar en `zb_storage` como "la persistencia interna del stack Zigbee", no como una base de datos propia que pueda interpretar libremente.

### 7.2 Que persiste el stack y que no conviene asumir

La documentacion publica dice que en `autostart` el stack carga parametros desde NVRAM y procede con el arranque, que puede implicar:

- `formation` para coordinador;
- `join` o `rejoin` segun el rol y el estado previo.

Por tanto, si `zb_storage` sigue intacta, es razonable esperar que el stack conserve al menos el estado persistente necesario para:

- saber si el nodo ya pertenecia a una red;
- reanudar o reconstruir el contexto de arranque de esa red;
- continuar con seguridad y parametros Zigbee persistentes.

Lo que no conviene asumir como contrato publico estable es el detalle exacto de bajo nivel:

- formato binario interno;
- claves internas concretas;
- disposicion exacta de tablas internas;
- o que una version futura del stack persista exactamente el mismo conjunto de estructuras.

Para la aplicacion, la regla sana es:

- confiar en el comportamiento observable del stack;
- no depender del layout interno de `zb_storage`;
- y persistir por separado cualquier dato de negocio propio.

### 7.3 Cache interna del stack frente a cache de la aplicacion

Cuando en este documento se habla de "cache", hay dos sentidos distintos.

#### Cache o estado interno del stack

El stack mantiene estado interno en RAM mientras esta ejecutandose, por ejemplo:

- scheduler y buffer pool;
- contexto de red activo;
- tablas y estructuras internas de Zigbee;
- estado transitorio de commissioning, routing, seguridad y entrega de mensajes.

Ese estado en RAM no debe confundirse con la persistencia de `zb_storage`.

Tras un reinicio:

- la RAM del stack desaparece;
- el stack vuelve a inicializar scheduler, buffers y estructuras de ejecucion;
- y despues carga, si existe, el estado persistente de NVRAM/`zb_storage`.

Es decir:

- la cache RAM del stack es volatil;
- `zb_storage` es persistente;
- y ambos juntos explican por que un coordinador puede "recordar la red" tras reiniciar aunque toda la RAM anterior ya no exista.

#### Cache de la aplicacion

La aplicacion puede mantener su propia cache, por ejemplo:

- lista de nodos por `ieee_addr`;
- descriptores descubiertos;
- atributos leidos o reportados;
- timestamps de actividad;
- clasificacion funcional del dispositivo;
- datos de negocio o integracion externa.

Esa cache no la gestiona el stack por ti.

Si la aplicacion no la persiste por su cuenta:

- se pierde al reiniciar el coordinador;
- aunque el stack siga pudiendo reanudar la red gracias a `zb_storage`.

Esa diferencia explica situaciones como esta:

- el coordinador reinicia;
- el stack reentra correctamente en la red;
- pero la app aun no ha reconstruido su modelo RAM de un dispositivo concreto;
- y el primer mensaje que recibe de ese nodo puede ser directamente un report ZCL.

### 7.4 `zb_storage`, `zb_fct` y configuracion de fabricacion

La API oficial tambien menciona `zb_fct` al hablar de seleccion de canales:

- si no se fija explicitamente la mascara de canales primaria, el stack puede escanear todos los canales permitidos;
- o leer configuracion de la zona NVRAM `zb_fct` si existe.

Conviene distinguir ambas zonas conceptualmente:

- `zb_storage`
  Persistencia operativa del stack Zigbee en ejecucion real.

- `zb_fct`
  Zona asociada a configuracion de fabricacion o parametros preinyectados.

No cumplen el mismo papel:

- `zb_storage` guarda estado de funcionamiento Zigbee entre reinicios;
- `zb_fct` sirve como fuente de configuracion de fabrica cuando procede.

### 7.5 Escenarios tipicos de arranque respecto a la persistencia

#### Arranque limpio sin estado previo

Si no hay estado util en `zb_storage`:

- el stack arranca sin contexto de red previo;
- la aplicacion suele iniciar `BDB` de formacion en un coordinador;
- y la red se crea de nuevo.

#### Reinicio con `zb_storage` intacta

Si `zb_storage` sigue presente:

- el stack puede cargar parametros persistidos al arrancar;
- el coordinador puede reanudar su pertenencia a la red;
- y la aplicacion recibira senales compatibles con esa reanudacion, como `DEVICE_REBOOT` o flujos equivalentes segun el caso.

#### `local reset`

`esp_zb_bdb_reset_via_local_action()`:

- hace que el dispositivo abandone la red actual;
- limpia los datos Zigbee persistentes;
- pero conserva el contador saliente NWK segun la nota oficial de la API;
- y deja el nodo en un estado casi de fabrica.

#### `factory reset`

`esp_zb_factory_reset()`:

- borra completamente `zb_storage`;
- reinicia el dispositivo;
- y obliga a tratar el siguiente arranque como un arranque sin persistencia Zigbee previa.

#### Borrado forzado al arrancar

La API oficial expone `esp_zb_nvram_erase_at_start(true)`.

Eso significa:

- borrar `zb_storage` en cada arranque antes de que el stack empiece;
- lo que equivale, en la practica, a impedir que el stack reutilice persistencia Zigbee previa;
- y es algo util para pruebas, pero normalmente no para un coordinador que deba mantener continuidad de red.

### 7.6 Implicaciones practicas para la aplicacion

- No confundas "el stack recuerda la red" con "la aplicacion recuerda los dispositivos".

- Si quieres conservar tu modelo de nodos, debes persistirlo aparte.

- No dependas del contenido binario de `zb_storage` como API publica.

- Si borras `zb_storage`, debes asumir:
  - perdida de continuidad Zigbee a nivel stack;
  - nueva formacion o nuevo proceso de red segun el rol;
  - y reconstruccion completa de la vision de red de la aplicacion.

- Si mantienes `zb_storage` pero no tu cache de aplicacion, debes asumir:
  - continuidad de red razonable a nivel Zigbee;
  - pero reconstruccion parcial o total del inventario RAM de nodos desde mensajes entrantes y consultas ZDO/ZCL.

## 8. Ejemplos de secuencias de mensajes

Estas secuencias son ejemplos tipicos y pedagogicos. El orden exacto puede variar segun el dispositivo remoto, el padre al que se asocie, si ya habia cache local, si hay reporting configurado y el modo de seguridad de la red.

### 8.1 Dispositivo nuevo que nunca ha estado en la red y se va a unir

Supongamos un sensor de temperatura factory-new que nunca ha pertenecido a esta red.

1. Coordinador -> red
   `esp_zb_bdb_open_network()` o `esp_zb_zdo_permit_joining_req()` para permitir joins.

2. Dispositivo -> red
   Busca una red compatible y solicita unirse.

3. Coordinador <- stack
   Puede aparecer `ESP_ZB_NWK_SIGNAL_DEVICE_ASSOCIATED`.

4. Coordinador <- stack
   Puede aparecer `ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED` cuando el Trust Center completa la autorizacion.

5. Coordinador <- stack
   Aparece `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE` con estado de join o rejoin inicial.

6. Dispositivo -> coordinador
   Envia `Device_annce`.

7. Coordinador <- stack
   Aparece `ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE` con:
   - `device_short_addr`
   - `ieee_addr`
   - `capability`

8. Coordinador -> dispositivo
   `esp_zb_zdo_node_desc_req()`

9. Dispositivo -> coordinador
   Respuesta en `esp_zb_zdo_node_desc_callback_t` con `esp_zb_af_node_desc_t`.

10. Coordinador -> dispositivo
    `esp_zb_zdo_active_ep_req()`

11. Dispositivo -> coordinador
    Respuesta en `esp_zb_zdo_active_ep_callback_t` con la lista de endpoints.

12. Coordinador -> dispositivo
    `esp_zb_zdo_simple_desc_req()` por cada endpoint de interes.

13. Dispositivo -> coordinador
    Respuesta en `esp_zb_zdo_simple_desc_callback_t` con perfil, device ID y clusters.

14. Coordinador -> dispositivo
    `esp_zb_zcl_read_attr_cmd_req()` para leer atributos de identidad o telemetria inicial, por ejemplo de `Basic` o `Temperature Measurement`.

15. Dispositivo -> coordinador
    `ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID`.

16. Coordinador -> dispositivo
    `esp_zb_zcl_config_report_cmd_req()` para pedir reporting de temperatura si el cluster lo soporta.

17. Dispositivo -> coordinador
    `ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID`.

Desde ese punto, el nodo ya puede pasar a regimen normal de reports.

### 8.2 Dispositivo que ha pasado horas fuera de la red

Supongamos un nodo que pertenecia a la red, pero ha estado apagado o fuera de alcance durante horas y vuelve.

1. Dispositivo -> red
   Intenta rejoin a la red conocida.

2. Coordinador <- stack
   Suele aparecer `ESP_ZB_ZDO_SIGNAL_DEVICE_UPDATE` con un estado de rejoin, por ejemplo:
   - `ESP_ZB_ZDO_STANDARD_DEV_SECURED_REJOIN`
   - o `ESP_ZB_ZDO_STANDARD_DEV_TC_REJOIN`

3. Dispositivo -> coordinador
   Puede enviar tambien `Device_annce`.

4. Coordinador <- stack
   Puede aparecer `ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE`.

5. Coordinador -> dispositivo
   Si ya conoce `ieee_addr`, `endpoints` y clusters, no necesita rehacer siempre toda la entrevista.
   Puede optar por una de estas politicas:
   - entrevista ligera: `node_desc_req` o `active_ep_req`
   - verificacion minima: `Read Attributes` de algun atributo conocido
   - ninguna entrevista inmediata y esperar al siguiente trafico ZCL

6. Si la aplicacion sospecha cambio de topologia o de direccion
   Puede emitir:
   - `esp_zb_zdo_node_desc_req()`
   - `esp_zb_zdo_active_ep_req()`
   - `esp_zb_zdo_simple_desc_req()`
   - o `esp_zb_zdo_mgmt_lqi_req()` para diagnostico

7. Dispositivo -> coordinador
   Reanuda sus reports o responde a lecturas.

La idea importante en este caso es que un rejoin no obliga a tratar el nodo como totalmente nuevo si la aplicacion ya conserva su identidad y su modelo.

### 8.3 Dispositivo de temperatura que reporta un cambio de temperatura

Supongamos que el coordinador ya configuro reporting sobre `Temperature Measurement`.

1. En un momento anterior
   El coordinador envio `esp_zb_zcl_config_report_cmd_req()` para el atributo `MeasuredValue`.

2. Dispositivo -> coordinador
   Cuando el valor cambia lo suficiente o expira el `max_interval`, envia un `Report Attributes`.

3. Coordinador <- stack
   El mensaje entra por `ESP_ZB_CORE_REPORT_ATTR_CB_ID` con `esp_zb_zcl_report_attr_message_t`.

4. La aplicacion extrae del mensaje
   - direccion origen
   - endpoint origen
   - cluster `Temperature Measurement`
   - atributo `MeasuredValue`
   - tipo y valor
   - metadatos como `TSN` o `RSSI` si los necesita

5. La aplicacion actualiza su cache local
   - valor de temperatura
   - `last_seen`
   - calidad de enlace si la mantiene

6. Opcionalmente, coordinador -> dispositivo
   Si detecta incoherencia, puede lanzar un `esp_zb_zcl_read_attr_cmd_req()` para confirmar el valor.

7. Dispositivo -> coordinador
   Llegaria `ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID`.

En operacion normal, para un sensor bien configurado, el camino habitual es el paso 2 -> 3 -> 5, sin polling constante.

### 8.4 Switch que cambia de estado y el coordinador acaba de arrancar sin haber oido aun a ese dispositivo

Supongamos que el coordinador se ha reiniciado, ha restaurado la red, pero su RAM de aplicacion todavia no ha visto trafico de ese switch concreto.

1. Coordinador <- stack
   Tras arrancar, el coordinador recupera la red con `ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT` o flujo equivalente.

2. El switch ya pertenece a la red
   Pero el coordinador todavia no lo ha vuelto a descubrir en esta sesion.

3. El switch cambia de estado local
   Por ejemplo de `OFF` a `ON`.

4. Dispositivo -> coordinador
   Si el switch implementa reporting del atributo `OnOff`, envia un `Report Attributes`.

5. Coordinador <- stack
   El primer mensaje que la aplicacion recibe de ese nodo puede ser directamente `ESP_ZB_CORE_REPORT_ATTR_CB_ID`, aunque todavia no haya recibido un `DEVICE_ANNCE` reciente de ese dispositivo.

6. La aplicacion observa
   - `src_address` corto
   - endpoint origen
   - cluster `On/Off`
   - atributo `OnOff`
   - nuevo valor

7. Como el nodo aun no esta en la cache de aplicacion de esta sesion
   El coordinador puede iniciar resolucion e entrevista a posteriori:
   - `esp_zb_zdo_ieee_addr_req()` para fijar identidad estable
   - `esp_zb_zdo_node_desc_req()`
   - `esp_zb_zdo_active_ep_req()`
   - `esp_zb_zdo_simple_desc_req()`

8. Dispositivo -> coordinador
   Va respondiendo en los callbacks ZDO correspondientes.

9. Coordinador -> dispositivo
   Si necesita confirmar estado o identidad funcional, puede enviar `esp_zb_zcl_read_attr_cmd_req()` sobre `OnOff` o `Basic`.

10. Dispositivo -> coordinador
    Responde via `ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID`.

La ensenanza principal de este ejemplo es que el primer mensaje util de un nodo no tiene por que ser `DEVICE_ANNCE`. Un coordinador puede recibir primero un mensaje ZCL y, a partir de ahi, decidir descubrir ese dispositivo.

### 8.5 Variante importante para switches basados en comandos y no en reporting

Muchos dispositivos tipo pulsador o mando no reportan un atributo `OnOff`, sino que envian comandos ZCL como `On`, `Off` o `Toggle` hacia sus destinos enlazados.

En ese caso, la secuencia cambia asi:

1. El switch cambia de estado o el usuario pulsa una tecla.
2. Dispositivo -> coordinador
   Envia un comando ZCL del cluster `On/Off`.
3. Coordinador <- stack
   El primer mensaje entra por el callback ZCL de comando aplicable, no por `ESP_ZB_CORE_REPORT_ATTR_CB_ID`.
4. Si la app no conoce aun ese nodo en RAM
   Puede igualmente lanzar `ieee_addr_req`, `node_desc_req`, `active_ep_req` y `simple_desc_req` para incorporarlo a su modelo.

## 9. Recomendaciones para no saturar la red

Estas recomendaciones no dependen de un firmware concreto. Son practicas de arquitectura y operacion utiles para cualquier coordinador.

- No conviertas la entrevista de un nodo en una rafaga continua.
  Espacia `Node Descriptor`, `Active EP`, `Simple Descriptor`, lecturas ZCL y `Configure Reporting`.

- No abras `permit join` mas tiempo del necesario.
  Ventanas cortas reducen trafico de comisionamiento accidental y ruido de red.

- Prefiere reporting a polling cuando el dispositivo lo soporte.
  Si un sensor puede reportar cambios, evita leerlo periodicamente sin necesidad.

- Usa polling lento como red de seguridad, no como fuente principal de datos.
  Un sondeo de salud puede existir, pero con periodos largos y jitter.

- Introduce `jitter` en reintentos y operaciones periodicas.
  Si muchos nodos comparten la misma cadencia, se generan picos evitables.

- Distingue entre timeout logico y timeout de red.
  El stack ya define timeouts base de varios ZDO (`5 * ESP_ZB_TIME_ONE_SECOND` en muchas peticiones). La aplicacion no deberia encadenar reintentos agresivos por debajo de esa escala.

- Usa backoff creciente.
  Tras un fallo, intenta por ejemplo `1x`, `2x`, `4x` y luego pasa a un periodo de observacion mas largo.

- No lances varias peticiones pesadas al mismo nodo en paralelo si puedes evitarlo.
  Es mejor un pipeline pequeno y ordenado por nodo que una tormenta de `Read Attributes`.

- Correlaciona peticiones con `TSN`.
  Sobre todo para ZCL. No des por buena una respuesta si no puedes asociarla a la transaccion correcta.

- Trata broadcast y `Mgmt_NWK_Update_req` como herramientas caras.
  Son utiles, pero impactan a mas de un nodo. Deben ser operaciones excepcionales o administrativas.

- Evita leer atributos que ya puedes inferir por descriptor o por cache fresca.
  Releer `Basic` o `Simple Descriptor` constantemente suele aportar poco valor.

- Define una politica de "nodo silencioso" antes de una politica de "nodo ausente".
  Es mejor degradar primero la confianza del dato que marcar el dispositivo como perdido demasiado pronto.

- Copia los datos de callback que vayas a conservar.
  No dependas del lifetime de buffers o punteros entregados por el stack mas alla del callback.

- Separa telemetria, control y mantenimiento.
  Si al mismo tiempo haces lecturas, reconfiguracion, binds y escaneos, prioriza.

- Haz idempotentes las operaciones de configuracion.
  Antes de reenviar `Configure Reporting`, decide si realmente hace falta.

- No reintentes indefinidamente.
  Un maximo razonable por fase evita que un nodo problematico monopolice la red.

- Mide antes de afinar.
  Cuenta `read ok/fail`, `report ok/fail`, latencia ZDO/ZCL, LQI y frecuencia de rejoins. Sin esas metricas es facil optimizar en la direccion equivocada.

## 10. Criterio practico para una app coordinador

Una estrategia robusta y muy general para usar el stack oficial es:

1. Detectar el nodo con `DEVICE_ANNCE` o `DEVICE_UPDATE`.
2. Resolver identidad estable (`IEEE` si hiciera falta).
3. Leer `Node Descriptor`.
4. Leer `Active EP`.
5. Leer `Simple Descriptor` por endpoint.
6. Leer solo los atributos ZCL necesarios.
7. Configurar reporting solo en los atributos utiles.
8. Mantener una cache local por `ieee_addr`.
9. Reintentar con backoff y jitter.
10. Usar `Mgmt_Lqi` y `Mgmt_NWK_Update` solo como diagnostico o administracion.

## 11. Fuentes oficiales

- Headers oficiales del componente:
  - `managed_components/espressif__esp-zigbee-lib/include/esp_zigbee_core.h`
  - `managed_components/espressif__esp-zigbee-lib/include/bdb/esp_zigbee_bdb_commissioning.h`
  - `managed_components/espressif__esp-zigbee-lib/include/bdb/esp_zigbee_bdb_touchlink.h`
  - `managed_components/espressif__esp-zigbee-lib/include/zdo/esp_zigbee_zdo_common.h`
  - `managed_components/espressif__esp-zigbee-lib/include/zdo/esp_zigbee_zdo_command.h`
  - `managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_common.h`
  - `managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_core.h`
  - `managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_command.h`
  - `managed_components/espressif__esp-zboss-lib/include/zboss_api_af.h`

- Documentacion oficial de Espressif:
  - <https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c6/api-reference/zcl/index.html>
  - <https://docs.espressif.com/projects/esp-zigbee-sdk/en/latest/esp32c3/api-reference/zdo/esp_zigbee_zdo_command.html>

