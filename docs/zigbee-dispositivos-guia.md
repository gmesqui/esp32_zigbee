# Guia de manejo de dispositivos Zigbee en este proyecto

## Alcance

Este documento describe el manejo de dispositivos Zigbee tal y como esta implementado hoy en el proyecto.

Quedan fuera de alcance:

- La capa de transporte hacia clientes externos
- Ethernet, SNTP y conectividad IP
- La presentacion hacia Home Assistant o clientes externos

Se incluye aquello que impacta en el comportamiento Zigbee del coordinador aunque hoy se invoque desde otra capa, por ejemplo el envio de ordenes ZCL.

## Archivos fuente principales

- `main/main.c`: orden de inicializacion de subsistemas
- `main/zigbee_core.c`: arranque de stack Zigbee, signal handler y action handler
- `main/device_manager.h/.c`: tabla de dispositivos en RAM, estados, presencia y online/offline
- `main/nvs_cache.h/.c`: persistencia de dispositivos conocidos
- `main/device_interview.h/.c`: entrevista del dispositivo, redescubrimiento y lectura de binding table
- `main/report_config.h/.c`: configure-reporting e IAS CIE address
- `main/zcl_handler.h/.c`: recepcion de atributos, cache ZCL, IAS y resolucion de NWK desconocido
- `main/zb_events.h/.c`: bus interno de eventos entre la capa Zigbee y consumidores externos

## Resumen de arquitectura

El proyecto no usa una estructura publica del SDK para modelar dispositivos remotos. En su lugar mantiene un modelo propio en RAM, persistido parcialmente en NVS.

Las tres piezas mas importantes son:

- `device_manager`: verdad local del dispositivo
- `device_interview`: descubrimiento activo mediante ZDO y lecturas ZCL
- `zcl_handler`: observacion reactiva mediante reports, respuestas y comandos entrantes

La consecuencia practica es que un dispositivo puede ser enriquecido por varias vias al mismo tiempo:

- anuncios y senales ZDO
- entrevista activa
- reports ZCL espontaneos
- respuestas a lecturas lanzadas por el coordinador
- mensajes IAS
- refresh de startup sobre dispositivos ya conocidos

## Modelo de datos del dispositivo

Cada dispositivo se representa con `device_record_t`.

Campos persistidos:

- Identidad: `ieee_addr`, `nwk_addr`, `friendly_name`
- Node descriptor: `node_desc_flags`, `mac_capability_flags`, `manufacturer_code`
- Power descriptor: `power_desc_flags`
- Identidad ZCL Basic: `manufacturer`, `model`, `power_source`
- Topologia funcional: `endpoint_count`, endpoints, clusters y bindings
- `reporting_configured`
- Derivado persistido: `is_sleepy`

Campos solo de runtime:

- `state`
- `online`
- `last_seen_ms`
- `last_rssi`, `last_lqi`, `radio_metrics_valid`
- contadores de telemetria
- `last_probe_ms`
- `binding_refresh_active`
- `dirty`, `in_use`
- `slot_generation`

### Estados del dispositivo

El enum `device_state_t` tiene cinco estados:

- `DEV_STATE_NEW`: recien descubierto, aun sin entrevista
- `DEV_STATE_INTERVIEWING`: entrevista en curso
- `DEV_STATE_INTERVIEWED`: descriptores y datos base ya conocidos
- `DEV_STATE_CONFIGURED`: reporting configurado
- `DEV_STATE_FAILED`: entrevista agotada tras reintentos

Estos estados expresan conocimiento funcional del dispositivo, no disponibilidad de radio. La disponibilidad se gestiona por separado mediante `online`.

## Persistencia en NVS

La persistencia la implementa `nvs_cache`.

### Que se guarda

Se guarda el conocimiento duradero del dispositivo:

- direccion IEEE
- direccion NWK actual conocida
- nombre amigable
- descriptors y datos Basic
- endpoints y clusters
- bindings recopilados
- flag `reporting_configured`

### Que no se guarda

No se guardan:

- `online`
- `last_seen_ms`
- `last_lqi`, `last_rssi`
- estado de entrevista en curso
- cache de atributos ZCL de `zcl_handler`
- buffer de pending attrs

### Comportamiento al restaurar tras reinicio del coordinador

Cuando `nvs_cache_load()` reconstruye la tabla:

- el dispositivo se recupera con sus descriptors y endpoints
- `reporting_configured` se restaura tal cual estaba
- `state` se restablece como `DEV_STATE_CONFIGURED` si `reporting_configured` era true, o `DEV_STATE_INTERVIEWED` si no
- `online` se fuerza a `false`

Esto es importante: tras un reinicio del coordinador, los dispositivos conocidos no se consideran online hasta que el coordinador reciba trafico suyo o una senal Zigbee que provoque `dm_touch()` o `dm_set_online(true)`.

## Arranque del coordinador

Secuencia simplificada:

1. `app_main()` inicializa NVS, bus de eventos, `device_manager`, `nvs_cache`, `zcl_handler`, `device_interview` y finalmente `zigbee_core`.
2. `zigbee_core` crea la tarea Zigbee y registra:
- signal handler para eventos del stack
- action handler para mensajes ZCL
3. Si el stack forma red nueva o restaura una ya existente, programa el mantenimiento periodico y llama a `di_startup_probe_known_devices()`.

## Caso 1: join de un dispositivo nuevo

### Evento de entrada principal

El flujo habitual empieza en `ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE`:

- se extraen IEEE y NWK
- `dm_get_or_create()` crea o recupera el slot
- se actualiza la NWK si cambio
- se marca `online=true`

### Decidir si hay entrevista

Si el dispositivo esta en:

- `DEV_STATE_NEW`
- `DEV_STATE_FAILED`

se encola una entrevista completa con `di_enqueue()`.

Si el dispositivo ya existia y estaba en otro estado, no se rehace la entrevista completa salvo que falte `reporting_configured`.

### Entrevista

La entrevista corre integramente dentro del contexto Zigbee usando `esp_zb_scheduler_alarm()`. No hay una tarea dedicada adicional.

Orden actual de pasos:

1. `NODE_DESC`
2. `POWER_DESC`
3. `ACTIVE_EP`
4. `SIMPLE_DESC` por endpoint
5. `READ_BASIC`
6. `READ_POWER_CFG` si existe cluster `0x0001`
7. `CONFIG_REPORT`

### Reintentos y tolerancia a fallos

- `NODE_DESC` y `ACTIVE_EP` reintentan hasta 3 veces con backoff `1000 << retry`
- `POWER_DESC` es no fatal
- `SIMPLE_DESC` fallido en un endpoint no aborta toda la entrevista; ese endpoint se omite y se continua

### Resultado de la entrevista

Si termina bien:

- `rc_configure_device()` envia los `Configure Reporting`
- el dispositivo pasa a `DEV_STATE_CONFIGURED`
- se guarda inmediatamente en NVS
- se lanza lectura de binding table con `Mgmt_Bind_req`

Si fracasa:

- el dispositivo pasa a `DEV_STATE_FAILED`
- queda registrado para futuros intentos

## Caso 2: redescubrimiento de dispositivos ya joined tras reinicio del coordinador

Este es uno de los flujos mas importantes del proyecto.

### Situacion inicial tras reboot

Tras `nvs_cache_load()`:

- el coordinador recuerda la identidad y descriptores del dispositivo
- el dispositivo queda `online=false`
- la cache de atributos ZCL no se restaura

### Startup probe de dispositivos conocidos

Al formar o restaurar la red, `zigbee_core` llama a `di_startup_probe_known_devices()`.

Este startup probe:

- recorre los dispositivos restaurados
- solo considera dispositivos con `state >= DEV_STATE_INTERVIEWED`
- excluye dispositivos `is_sleepy`
- lanza lecturas ZCL sobre clusters conocidos de cada endpoint
- intenta tambien refrescar la binding table

Objetivo:

- recuperar disponibilidad
- reconstruir parte del estado ZCL en cache
- obtener de nuevo atributos como `state`, `brightness`, medidas, bateria, IAS o potencia

### Consecuencia practica

Un dispositivo ya conocido no necesita reentrevistarse por completo tras el reboot del coordinador si la informacion persistida sigue siendo valida. Basta con que vuelva a hablar o responda a los reads del startup probe.

### Limitacion actual

El startup probe ignora dispositivos sleepy. Eso evita falso trafico innecesario, pero tambien implica que esos nodos permaneceran offline hasta que ellos mismos hablen o reaparezcan via otra senal.

## Caso 3: dispositivo ya conocido que rejoin a la red

Los caminos principales son:

- `DEVICE_ANNCE`
- `DEVICE_UPDATE`

Cuando el coordinador ya conoce el IEEE:

- actualiza la NWK si ha cambiado
- lo marca `online=true`
- no rehace la entrevista completa si ya estaba en `INTERVIEWED` o `CONFIGURED`

Excepcion:

- si `reporting_configured == false`, se vuelve a encolar configuracion mediante entrevista

Esto convierte el rejoin en una recuperacion ligera, no en una alta completa.

## Caso 4: llega trafico ZCL antes de identificar el dispositivo

Este proyecto soporta que el primer contacto util no sea un `DEVICE_ANNCE`.

Si entra un `Report Attributes` desde una NWK desconocida:

- `zcl_handler` guarda temporalmente el atributo en `g_pending`
- lanza `di_trigger_ieee_resolve(nwk)`
- cuando llega `IEEE_ADDR_RSP`, el sistema busca o crea el dispositivo por IEEE
- despues hace `zcl_pending_attr_replay()`

Esto permite no perder el primer atributo aunque el coordinador aun no conozca la correspondencia NWK -> IEEE.

## Caso 5: leave real del dispositivo

En `LEAVE_INDICATION`, si `rejoin == 0`:

- se marca offline
- se cancelan entrevista y probe pendientes con `di_forget_device()`
- se elimina la cache ZCL con `zcl_forget_device()`
- se elimina el slot del `device_manager`
- se borra la entrada de NVS

Es un borrado completo del conocimiento local del dispositivo.

Si `rejoin != 0`, no se hace esa eliminacion destructiva.

## Disponibilidad online/offline

La disponibilidad se actualiza por dos mecanismos distintos.

### 1. Transiciones inmediatas por eventos del stack

Se marca online cuando ocurre alguno de estos hechos:

- `DEVICE_ANNCE`
- `DEVICE_UPDATE`
- `dm_touch()` desde trafico ZCL o lecturas

Se marca offline inmediatamente cuando ocurre:

- `LEAVE_INDICATION` sin rejoin
- `DEVICE_UNAVAILABLE`

### 2. Timeout de presencia periodico

Cada `10 s`, `maintenance_alarm()` ejecuta:

- `nvs_cache_save_dirty()`
- `dm_check_presence()`

`dm_check_presence()` usa umbrales fijos:

- always-on: `320 s`
- sleepy: `3620 s`

La regla es:

- `presence_timeout = max_interval_reporting_efectivo + 20 s`

Si se supera ese tiempo desde `last_seen_ms`, se marca offline por inactividad.

### Importante

En la practica, para la mayoria de cambios cortos de disponibilidad manda el signal `DEVICE_UNAVAILABLE`, no el timeout horario. El timeout es una red de seguridad lenta.

## Como se marca un dispositivo como "visto"

`dm_touch()` hace tres cosas:

- actualiza `last_seen_ms`
- actualiza `last_lqi` y `last_rssi` si vienen metricas validas
- si el dispositivo estaba offline, lo pasa a online y emite evento de disponibilidad

`dm_touch()` es llamado desde:

- `zcl_on_report_attr()`
- `zcl_on_read_attr_resp()`
- `zcl_on_ias_enroll_req()`
- `zcl_on_ias_zone_status()`

Por tanto, cualquier trafico ZCL entrante puede "revivir" logicamente un dispositivo.

## Entrevista en detalle

### Cola y concurrencia

- solo hay una entrevista activa a la vez
- la cola `IQUEUE_SIZE` es de 8 dispositivos
- si la cola se llena, se descarta el nuevo enqueue y queda log

### Proteccion frente a reutilizacion de slots

Las callbacks ZDO usan un `user_ctx` que codifica:

- indice de slot
- `slot_generation`

Esto evita asociar respuestas tardias a un dispositivo distinto que reutilice el mismo slot tras un borrado.

### Informacion que aporta cada paso

`NODE_DESC`:

- capacidades MAC
- `manufacturer_code`
- derivacion de `is_sleepy`

`POWER_DESC`:

- power descriptor crudo persistido

`ACTIVE_EP`:

- lista de endpoints

`SIMPLE_DESC`:

- profile id
- device id
- version
- clusters in/out

`READ_BASIC`:

- manufacturer string
- model string
- power_source

`READ_POWER_CFG`:

- battery voltage
- battery percentage

`CONFIG_REPORT`:

- envio de configure-reporting segun la tabla del proyecto

## Como se configura el reporting para cada dispositivo

Esta seccion describe el flujo real que sigue el proyecto para configurar reporting en un dispositivo concreto.

### Momento en que se dispara

La configuracion de reporting se lanza en dos caminos principales:

- al final de una entrevista normal, en el paso `ISTATE_CONFIG_REPORT`
- de forma asincrona sobre un dispositivo ya conocido cuando se llama a `rc_configure_device_async()`

En la practica, el camino habitual es el primero:

1. el dispositivo entra en entrevista
2. se descubren descriptors, endpoints y clusters
3. cuando el coordinador ya sabe que clusters tiene cada endpoint, entra en `CONFIG_REPORT`
4. ahi se llama a `rc_configure_device(dev)`

### Que necesita saber antes de configurar reporting

El coordinador no puede configurar reporting "a ciegas". Antes necesita haber descubierto al menos:

- la lista de endpoints del dispositivo
- los input clusters de cada endpoint

Por eso la configuracion de reporting va despues de:

- `ACTIVE_EP`
- `SIMPLE_DESC`

Sin esa informacion no sabria en que endpoint enviar cada `Configure Reporting`.

### Algoritmo por dispositivo

Cuando se llama `rc_configure_device(dev)`, el flujo es este:

1. recorre todos los endpoints almacenados en `dev->endpoints`
2. para cada endpoint recorre la tabla local `k_report_table`
3. comprueba si ese endpoint tiene el `cluster_id` de la entrada como input cluster
4. si no lo tiene, salta esa entrada
5. si lo tiene, construye y envia un `Configure Reporting` para ese `cluster/attr`
6. repite el proceso hasta agotar todos los endpoints y todas las entradas aplicables

Esto implica que la configuracion final depende de la interseccion entre:

- lo que el coordinador quiere configurar
- lo que ese dispositivo declara soportar

### Numero de comandos enviados

No se envia un unico comando global por dispositivo.

El proyecto envia:

- un comando `Configure Reporting` por cada combinacion aplicable de `endpoint + cluster + atributo`

Ejemplo conceptual:

- si un dispositivo tiene un endpoint con `OnOff` y `Level`, recibira al menos dos configuraciones
- si ademas tiene `Color Control`, recibira varias mas, una por cada atributo de color definido en la tabla
- si tiene dos endpoints con el mismo cluster, cada endpoint se configura por separado

### Construccion tecnica del comando

Para cada entrada aplicable, `send_config_report()` construye:

- `direction = SEND`
- `attributeID = attr_id`
- `attrType = attr_type`
- `min_interval`
- `max_interval`
- `reportable_change`

Detalles importantes de la implementacion:

- `reportable_change` no se pasa siempre como entero bruto
- el SDK espera un puntero tipado segun el tipo ZCL del atributo
- por eso el codigo convierte el valor a `uint8`, `uint16` o `int16` antes de llamar a `esp_zb_zcl_config_report_cmd_req()`
- para atributos discretos, ese campo se deja en `NULL`

### Caso especial IAS durante la configuracion

Si la entrada corresponde a:

- cluster `0x0500`
- atributo `0x0002`

el coordinador hace primero:

- `rc_write_ias_cie_address(dev->nwk_addr, ep->endpoint_id)`

y despues envia el `Configure Reporting`.

La idea es que el sensor IAS ya conozca al coordinador como CIE antes de empezar a trabajar con su estado de zona.

### Que ocurre al terminar de enviar

Cuando `rc_configure_device(dev)` termina de recorrer todas las entradas aplicables:

- logea cuantas configuraciones ha enviado
- si se envio al menos una, abre una sesion de validacion con:
- `report_cfg_expected`
- `report_cfg_received`
- `report_cfg_failed`
- `dev->reporting_configured = false`
- `dev->dirty = true`

Eso significa que el dispositivo queda en estado de "reporting pendiente de validar" hasta que lleguen todas las respuestas esperadas o venza el timeout de esa fase.

### Que pasa con la respuesta del dispositivo

Las respuestas entran por:

- `ESP_ZB_CORE_CMD_REPORT_CONFIG_RESP_CB_ID`

y se procesan en:

- `rc_on_config_resp()`

Ese handler:

- localiza el dispositivo por NWK
- actualiza la sesion runtime de validacion del reporting
- recorre la lista de resultados por atributo
- logea `FAIL` si algun atributo fue rechazado
- logea `OK` si no encontro fallos en esa respuesta

### Como debe interpretarse funcionalmente

En la version actual del proyecto:

- el exito de la entrevista si espera a validar las respuestas de `Configure Reporting`
- `reporting_configured` solo pasa a `true` cuando se han recibido todas las respuestas esperadas y ninguna ha fallado
- si faltan respuestas o alguna llega con error, la entrevista termina como fallo
- al final queda un log explicito `INTERVIEW_FINAL ... SUCCESS/FAILED` con:
- respuestas recibidas frente a esperadas
- numero de fallos
- si hubo timeout o no

Por tanto, para un dispositivo concreto `reporting_configured == true` ya significa "reporting validado", no solo "reporting intentado".

## Tabla de reporting del proyecto

Esta tabla no es una lista de dispositivos, sino una lista de atributos ZCL que el coordinador intenta dejar configurados para reporting.

Cada entrada tiene la forma:

- `cluster_id/attr_id`

Su significado es:

- `cluster_id`: funcionalidad Zigbee a la que pertenece el dato
- `attr_id`: atributo concreto dentro de ese cluster

Si durante la entrevista o en un redescubrimiento el coordinador detecta que un endpoint tiene ese cluster como input cluster, intentara enviar un `Configure Reporting` para ese atributo.

Dicho de otra forma: esta tabla define que datos esperamos que el dispositivo nos envie espontaneamente cuando cambien o cuando se cumpla su `max_interval`, sin tener que leerlos manualmente todo el tiempo.

### Que hace realmente `Configure Reporting`

Cuando el coordinador envia `Configure Reporting` a un atributo, le esta diciendo al dispositivo remoto:

- cuando es el intervalo minimo entre dos reports del mismo atributo
- cual es el intervalo maximo sin reportar, aunque el valor no cambie
- que delta minimo debe existir antes de reportar, si el tipo de dato es analogico

En esta implementacion, cada entrada de la tabla interna define:

- `cluster_id`
- `attr_id`
- `attr_type`
- `min_interval`
- `max_interval`
- `reportable_change`

Significado practico:

- `min_interval`: evita floods de reports demasiado frecuentes
- `max_interval`: fuerza un refresh periodico aunque el valor no cambie
- `reportable_change`: umbral minimo de cambio para atributos analogicos

Ejemplos:

- On/Off no necesita `reportable_change`, porque es un atributo discreto
- temperatura si usa `reportable_change`, para no reportar por variaciones minusculas
- el `max_interval` ya no depende del atributo para esta decision, sino del tipo de dispositivo

### Como se eligen en este proyecto `min_interval`, `max_interval` y `reportable_change`

En la implementacion actual:

- `min_interval` y `reportable_change` se definen manualmente en `k_report_table`
- `max_interval` se ajusta por tipo de dispositivo:
- always-on: `300 s`
- sleepy: `3600 s`

Eso significa que la decision responde a una politica fija del coordinador, basada en:

- el tipo de dato
- la velocidad esperable de cambio
- el coste de trafico que estamos dispuestos a aceptar
- la utilidad operativa del dato si permanece mucho tiempo sin refrescar

El criterio practico actual es este:

- atributos discretos o de evento: `min_interval` bajo, `reportable_change = 0`
- atributos analogicos rapidos: `min_interval` corto y `reportable_change` pequeno
- atributos analogicos lentos: `min_interval` algo mayor y `reportable_change` moderado
- atributos de bateria: `min_interval` largo, pero con el mismo `max_interval` base segun si el dispositivo es sleepy o always-on

### Regla mental por tipo de atributo

`min_interval` responde a:

- "cuanto quiero frenar reportes demasiado seguidos"

`max_interval` responde a:

- "cuanto tiempo maximo acepto estar sin noticias de este atributo"

`reportable_change` responde a:

- "cuanto debe cambiar el valor para que merezca la pena reportarlo"

### Ejemplos reales de la tabla del proyecto

- On/Off `0x0006/0x0000`: `min=0`, `max=300/3600`, `change=0`
- Level `0x0008/0x0000`: `min=1`, `max=300/3600`, `change=1`
- Temperature `0x0402/0x0000`: `min=10`, `max=300/3600`, `change=10`
- Humidity `0x0405/0x0000`: `min=10`, `max=300/3600`, `change=50`
- Pressure `0x0403/0x0000`: `min=10`, `max=300/3600`, `change=10`
- Illuminance `0x0400/0x0000`: `min=10`, `max=300/3600`, `change=500`
- Battery voltage `0x0001/0x0020`: `min=3600`, `max=300/3600`, `change=1`
- Battery percentage `0x0001/0x0021`: `min=3600`, `max=300/3600`, `change=2`
- IAS Zone status `0x0500/0x0002`: `min=0`, `max=300/3600`, `change=0`
- Active power `0x0B04/0x050B`: `min=5`, `max=300/3600`, `change=10`

### Como leer esos ejemplos

On/Off:

- se quiere reaccion inmediata a cambios
- no hay umbral de cambio porque el atributo es booleano
- se permite que el dispositivo reporte en cuanto cambie

Level:

- se evita flood continuo con `min_interval = 1`
- un cambio minimo de una unidad ya es suficiente para reportar

Temperature:

- el valor ZCL suele venir en centesimas de grado
- `change = 10` equivale aproximadamente a `0.10 C`
- asi se evita reportar ruido o microfluctuaciones

Humidity:

- el valor ZCL suele venir en centesimas de porcentaje
- `change = 50` equivale aproximadamente a `0.50 %`

Pressure:

- en este proyecto se interpreta con escala `valor / 10`
- `change = 10` equivale aproximadamente a `1.0` unidades de la escala usada

Illuminance:

- se usa un umbral bastante mayor porque este atributo puede variar mucho y generar mucho trafico
- ademas luego el proyecto convierte el valor ZCL a lux al decodificarlo

Battery voltage:

- cambios lentos
- refresco espaciado
- `change = 1` equivale a un paso de `100 mV` en la interpretacion actual

Battery percentage:

- el atributo suele venir en pasos de `0.5 %`
- `change = 2` equivale aproximadamente a `1 %`

IAS Zone status:

- es un bitmap de evento
- no tiene sentido aplicar umbral analogico
- interesa que los cambios de alarma lleguen sin retardo artificial

Active power:

- es una magnitud analogica con cambios potencialmente frecuentes
- `min = 5` limita el ritmo de reports
- en la interpretacion actual `change = 10` equivale aproximadamente a `1.0 W`

### Lo importante de cara a mantenimiento

Estos valores no son "los correctos segun Zigbee" en abstracto. Son una eleccion de compromiso.

Se pueden revisar en funcion de:

- saturacion de red
- frecuencia real de cambios observada
- sensibilidad deseada para automatizaciones
- autonomia de dispositivos a bateria
- ruido o granularidad real del sensor

En otras palabras: el proyecto usa una politica de reporting razonable por defecto, pero no una politica adaptativa por fabricante, modelo o tipo concreto de dispositivo.

### Como decide el coordinador si aplicar una entrada

El coordinador no envia toda la tabla a todos los dispositivos.

Para cada endpoint del dispositivo:

- recorre la tabla local de reporting
- comprueba si el endpoint tiene ese cluster como input cluster
- solo entonces envia el `Configure Reporting` correspondiente

Esto significa que la tabla es una politica local del coordinador, no una descripcion del dispositivo.

Los atributos configurados hoy son estos:

- On/Off `0x0006/0x0000`: estado binario del actuador. `0 = OFF`, `1 = ON`.
- Level `0x0008/0x0000`: nivel de brillo o nivel generico del cluster Level Control. Normalmente se usa en rango `0..254`.
- Color control `0x0300/0x0000`, `0x0001`, `0x0007`: tres atributos distintos del cluster Color Control.
- `0x0300/0x0000`: hue
- `0x0300/0x0001`: saturation
- `0x0300/0x0007`: color temperature en mireds
- Temperature `0x0402/0x0000`: temperatura medida. En ZCL suele venir como entero con escala `valor / 100`.
- Humidity `0x0405/0x0000`: humedad relativa. En ZCL suele venir como entero con escala `valor / 100`.
- Pressure `0x0403/0x0000`: presion medida. En este proyecto se interpreta como entero con escala `valor / 10`.
- Illuminance `0x0400/0x0000`: iluminancia medida. El valor ZCL no es lux directo; el proyecto lo convierte a lux al decodificarlo.
- Occupancy `0x0406/0x0000`: ocupacion detectada. El bit 0 indica si hay presencia o no.
- Battery `0x0001/0x0020`, `0x0021`: dos atributos del cluster Power Configuration.
- `0x0001/0x0020`: battery voltage, normalmente en pasos de `100 mV`
- `0x0001/0x0021`: battery percentage remaining, normalmente en pasos de `0.5 %`
- IAS Zone status `0x0500/0x0002`: bitmap de estado de una zona IAS. Puede reflejar alarma, tamper, bateria baja y otros flags.
- Active power `0x0B04/0x050B`: potencia activa del cluster Electrical Measurement. En este proyecto se interpreta como entero con escala `valor / 10`.

La idea funcional de cada grupo es:

- actuadores: `On/Off`, `Level`, `Color Control`
- sensores ambientales: `Temperature`, `Humidity`, `Pressure`, `Illuminance`
- sensores de presencia o estado: `Occupancy`, `IAS Zone status`
- energia: `Battery`, `Active power`

Que un atributo aparezca en esta tabla no garantiza por si solo que todos los dispositivos lo soporten o acepten reporting. Solo significa que, si ese cluster existe en el endpoint, el coordinador intentara configurarlo.

### Comportamiento especial para IAS

Antes de configurar reporting de `IAS Zone status`, el coordinador escribe su propia IEEE en `IAS_CIE_Address (0x0010)`.

Ademas, cuando llega un `IAS Enroll Request`, el coordinador responde con:

- `enroll_rsp_code = SUCCESS`
- `zone_id = 1`

### Como se interpreta el resultado de la configuracion

Hay que distinguir dos cosas:

- el envio del `Configure Reporting`
- la aceptacion real por parte del dispositivo

En el proyecto actual:

- `rc_configure_device()` envia los comandos segun la tabla local
- `rc_on_config_resp()` logea las respuestas `REPORT_CFG_RSP`
- `reporting_configured` solo pasa a `true` cuando se valida la sesion completa de respuestas
- la entrevista no termina con exito hasta cerrar esa validacion

Por eso conviene leer esta fase como:

- "el coordinador envio y valido correctamente el reporting"

## Binding table y bindings

### Que es un binding

Un binding Zigbee es una asociacion persistente dentro del propio dispositivo remoto.

Conceptualmente responde a esta idea:

- cuando cambie el cluster `X` del endpoint `A`, envia automaticamente ese trafico a este destino

Ese destino puede ser:

- un grupo Zigbee
- otro dispositivo concreto por IEEE + endpoint

Un binding no es un atributo ZCL ni una regla de transporte externo. Es informacion propia del nodo Zigbee, almacenada en su binding table.

### Que informacion contiene cada binding

En este proyecto, cada binding recuperado se guarda como:

- `src_endpoint`
- `cluster_id`
- `dst_addr_mode`
- `dst_group_addr` o `dst_ieee_addr`
- `dst_endpoint`

Lectura funcional:

- `src_endpoint`: desde que endpoint del nodo nace la relacion
- `cluster_id`: para que cluster aplica
- `dst_addr_mode`: si el destino es grupo o dispositivo IEEE
- `dst_group_addr`: grupo destino cuando aplica
- `dst_ieee_addr`: IEEE destino cuando aplica
- `dst_endpoint`: endpoint destino cuando aplica

### Para que usa el proyecto la binding table

El proyecto no usa la binding table para rutear sus propios comandos salientes. La usa como conocimiento adicional del dispositivo.

Sirve para saber:

- si el nodo tiene automatizaciones Zigbee directas hacia otros nodos
- si publica a grupos
- si tiene enlaces de control directos configurados fuera del coordinador

Eso ayuda a entender comportamientos del dispositivo que no dependen del bridge.

### Como se obtiene

La tabla de bindings se consulta activamente con `Mgmt_Bind_req`.

Se lanza en dos situaciones:

- al terminar una entrevista correcta
- durante startup probe o tras resolver IEEE de un dispositivo conocido

### Como se descarga y guarda

La descarga de bindings:

- soporta paginacion por `start_index`
- limpia los bindings almacenados cuando `index == 0`
- guarda group bindings y IEEE bindings
- persiste el resultado final en NVS

El flujo es este:

1. Se envia `Mgmt_Bind_req` con `start_index = 0`.
2. Si la respuesta trae varias entradas pero indica que hay mas, se vuelve a pedir la pagina siguiente.
3. Si la primera pagina llega bien, el proyecto borra antes los bindings previos almacenados localmente.
4. Cada registro remoto se traduce al modelo `binding_record_t`.
5. Cuando termina la ultima pagina, el resultado se persiste en NVS.

### Limitaciones actuales

- depende totalmente de que el dispositivo responda `Mgmt_Bind_rsp`
- si el nodo no responde, no se reconstruye nada por inferencia
- si falla el refresh actual, se conserva lo ultimo conocido que estuviera persistido
- el proyecto almacena un maximo de `MAX_BINDINGS_PER_EP = 8` bindings por endpoint

Si la respuesta falla:

- se corta el refresh actual
- se conserva la informacion persistida anterior si existia

## Recepcion de atributos ZCL

### Report Attributes

`zcl_on_report_attr()`:

- logea `RX RAW`
- resuelve el dispositivo por NWK
- si no lo encuentra, guarda pending attr y lanza `IEEE_ADDR_REQ`
- si lo encuentra, actualiza presencia con `dm_touch()`
- procesa el atributo y actualiza cache
- emite evento de cambio si el valor realmente cambio

### Read Attributes Response

`zcl_on_read_attr_resp()`:

- marca presencia con `dm_touch()`
- procesa cada atributo de la respuesta
- distingue exito y fallo por atributo
- marca atributos `unsupported`
- emite evento solo si hubo cambio real

## Cache ZCL local

`zcl_handler` mantiene una cache local de atributos escalados o crudos.

Objetivos:

- recordar el ultimo valor conocido
- detectar cambios reales frente a repetidos
- construir estado agregado
- enriquecer rapidamente dispositivos ya conocidos

La cache:

- no se persiste en NVS
- tiene tamano maximo `MAX_ATTR_CACHE = 128`
- si se llena, expulsa la entrada mas antigua

## Manejo de atributos no soportados

Cuando una respuesta `Read Attributes` devuelve `UNSUP_ATTRIB`:

- se registra ese atributo en una cache de no soportados

Uso practico:

- evitar relanzar probes innecesarios para atributos que ya sabemos que el nodo no soporta
- al reentrevistar un dispositivo se limpian esos marcadores con `zcl_clear_unsupported_attrs()`

## Sonda para dispositivos sleepy

Cuando un nodo sleepy reporta algo, `maybe_probe_sleepy_device()` puede aprovechar esa ventana despierta para pedir atributos que falten.

Condiciones:

- solo para `is_sleepy == true`
- se limita con un minimo de 2 s entre probes
- considera la tabla `k_sleepy_probe_table`

Si no faltan atributos pero no hay metricas de radio validas:

- se lanza una lectura del atributo reportado solo para forzar una respuesta y obtener RSSI/LQI

## IAS en el proyecto

### Que es IAS

IAS significa `Intruder Alarm Systems`.

En Zigbee, esta familia de clusters se usa para sensores de seguridad y eventos relacionados con alarma, por ejemplo:

- contacto de puerta/ventana
- movimiento
- tamper
- bateria baja
- zonas de alarma

En este proyecto la parte relevante es `IAS Zone`, no `IAS ACE` ni `IAS WD`.

### Rol del coordinador frente a un sensor IAS

Para que un sensor IAS envie eventos correctamente, el coordinador necesita actuar como destino CIE.

En el flujo actual eso implica:

- escribir su propia IEEE en `IAS_CIE_Address`
- aceptar el `Enroll Request`
- recibir despues los cambios de estado de zona

### Flujo IAS implementado

El flujo completo queda asi:

1. Durante la configuracion de reporting, si el endpoint soporta `IAS Zone`, el coordinador escribe antes `IAS_CIE_Address (0x0010)`.
2. El dispositivo puede enviar `IAS Enroll Request`.
3. `zcl_on_ias_enroll_req()` responde con `SUCCESS` y `zone_id = 1`.
4. A partir de ahi el dispositivo puede enviar `IAS Zone Status Change Notification`.
5. El coordinador convierte internamente ese evento a una actualizacion del atributo `0x0500/0x0002`.

### Por que se normaliza como atributo

Las notificaciones `IAS Zone Status Change Notification` no pasan por `Report Attributes`, pero se normalizan internamente como si actualizaran:

- cluster `0x0500`
- attr `0x0002`
- type `bitmap16`

Asi el resto del pipeline reutiliza:

- cache de atributos
- logs `STATE`
- emision de eventos

### Que representa `0x0500/0x0002`

Ese atributo es un bitmap de estado de zona.

En la decodificacion actual del proyecto se interpretan al menos estos flags:

- bit `0x0001`: `ALARM1`
- bit `0x0002`: `ALARM2`
- bit `0x0004`: `TAMPER`
- bit `0x0008`: `BATT_LOW`

Ademas, al exportar estado agregado, el proyecto deriva:

- `contact`
- `tamper`
- `battery_low`

### Relacion entre IAS y reporting

Para IAS hay dos mecanismos que pueden convivir:

- `Configure Reporting` sobre el atributo `IAS Zone status`
- notificaciones especificas `IAS Zone Status Change Notification`

El proyecto soporta ambas cosas, pero la ruta mas importante en tiempo real es la notificacion IAS especifica, porque llega como comando dedicado y luego se normaliza al atributo de estado.

## Envio de ordenes Zigbee

La entrada de ordenes hacia Zigbee vive en `client_actions.c` y se reduce a tres primitivas:

- `esp_zb_zcl_on_off_cmd_req()`
- `esp_zb_zcl_level_move_to_level_with_onoff_cmd_req()`
- `esp_zb_zcl_read_attr_cmd_req()`

### Como se selecciona el endpoint

Para ordenes y lecturas puntuales:

- se busca el primer endpoint que tenga el cluster de entrada requerido

Ejemplos:

- On/Off usa el primer endpoint con cluster `0x0006`
- Brightness usa el primer endpoint con cluster `0x0008`
- Lecturas bajo demanda usan el primer endpoint que contenga el cluster solicitado

### Lo que esta fuera de este documento

No se detalla aqui la parse de mensajes ni la logica del futuro transporte de entrada. Lo relevante desde el punto de vista Zigbee es que el proyecto ya tiene un camino funcional para:

- enviar ordenes On, Off y Toggle
- enviar MoveToLevel con on/off
- forzar lecturas de atributos

## Bus interno de eventos

La capa Zigbee emite eventos internos neutrales para consumidores externos.

Tipos relevantes:

- `ZB_EVT_DEVICE_JOINED`
- `ZB_EVT_DEVICE_LEAVE`
- `ZB_EVT_INTERVIEW`
- `ZB_EVT_ATTR_CHANGED`
- `ZB_EVT_AVAILABILITY`
- `ZB_EVT_PERMIT_JOIN`

Esto desacopla la logica Zigbee del transporte de publicacion.

## Tiempos y valores importantes

- mantenimiento periodico: `10 s`
- timeout de presencia always-on: `320 s`
- timeout de presencia sleepy: `3620 s`
- arranque del siguiente paso de entrevista: tipicamente `200 ms`
- espera tras `READ_BASIC`: `1500 ms`
- espera tras `READ_POWER_CFG`: `1000 ms`
- reintentos de `NODE_DESC` y `ACTIVE_EP`: hasta 3 intentos
- backoff de reintento: `1000 << retry`
- intervalo minimo entre sleepy probes: `2000 ms`
- ventana de "probe reciente": `5000 ms`
- delay entre startup probes de dispositivos: `1200 ms`
- reprogramacion del startup probe si hay entrevista en curso: `1500 ms`

## Casos practicos resumidos

### Join nuevo

- entra `DEVICE_ANNCE`
- se crea slot y se marca online
- se hace entrevista
- se configura reporting
- se lee binding table
- se guarda en NVS

### Coordinador reiniciado, dispositivo ya conocido

- se restaura desde NVS como `INTERVIEWED` o `CONFIGURED`
- arranca `offline`
- el startup probe intenta leer atributos y bindings si es always-on
- cualquier trafico entrante posterior lo devuelve a `online`

### Rejoin de un conocido

- se actualiza NWK
- se marca online
- no se reentrevista salvo que falte reporting

### Primer mensaje del nodo es un report

- se guarda como pending attr
- se resuelve IEEE
- se reproduce el atributo sobre el dispositivo correcto

### Leave definitivo

- offline
- cancelacion de entrevistas/probes
- borrado de cache ZCL
- eliminacion de RAM y NVS

## Lectura operativa del sistema

La implementacion actual combina dos filosofias:

- descubrimiento activo al principio o tras reboot del coordinador
- mantenimiento reactivo basado en el trafico real del nodo

Eso la hace bastante robusta para dispositivos ya conocidos, incluso cuando la primera evidencia tras un reboot no es un `DEVICE_ANNCE`, sino un report, una respuesta o un mensaje IAS.
