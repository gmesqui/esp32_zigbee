# Observaciones y puntos de revision sobre el manejo Zigbee actual

## Objetivo

Este documento no propone cambios de codigo. Solo resume comportamientos reales del sistema actual que conviene tener presentes al revisar o evolucionar la implementacion.

## 1. La disponibilidad inmediata depende mucho de `DEVICE_UNAVAILABLE`

Hoy `ESP_ZB_ZDO_DEVICE_UNAVAILABLE` marca `offline` al instante.

Eso significa:

- la presencia puede oscilar rapido si el stack emite unavailable de forma transitoria
- el timeout largo de `dm_check_presence()` queda como mecanismo secundario
- un solo frame ZCL posterior puede devolver el dispositivo a `online`

Consecuencia:

- el modelo real de disponibilidad a corto plazo es "event-driven", no "timeout-driven"

## 2. `online` y `state` son conceptos independientes

El sistema separa correctamente:

- conocimiento del dispositivo: `NEW`, `INTERVIEWED`, `CONFIGURED`, etc.
- disponibilidad actual: `online`

Esto evita perder descriptors por una caida de radio temporal.

Consecuencia positiva:

- un dispositivo conocido puede volver a la vida sin reentrevista completa

Consecuencia a vigilar:

- un dispositivo puede estar `CONFIGURED` y sin embargo `offline`

## 3. Tras reboot del coordinador no se restaura la cache ZCL

Se restaura:

- estructura del dispositivo
- endpoints
- clusters
- flag de reporting

No se restaura:

- cache de atributos

Consecuencia:

- el startup probe no solo sirve para presencia; tambien sirve para reconstruir estado funcional visible

## 4. El startup probe es deliberadamente conservador con sleepy devices

Los dispositivos sleepy restaurados desde NVS no se sondean durante el startup probe.

Ventaja:

- menos trafico inutil
- menos probabilidad de timeouts sistematicos

Coste:

- tras reboot del coordinador, esos nodos permanecen offline y sin estado fresco hasta que hablen por si mismos

## 5. El flujo soporta bien que el primer mensaje util sea ZCL y no ZDO

Esta es una decision acertada del diseno actual.

Si llega un report desde una NWK no conocida:

- no se descarta
- se bufferiza
- se resuelve IEEE
- se reinyecta sobre el dispositivo definitivo

Esto reduce mucho la fragilidad durante rejoins, cambios de NWK o reinicios del coordinador.

## 6. `reporting_configured` expresa "intencion enviada", no confirmacion exhaustiva

Tal y como esta implementado hoy:

- `rc_configure_device()` pone `dev->reporting_configured = true` justo despues de enviar los `Configure Reporting`
- `rc_on_config_resp()` solo logea fallos o exito, pero no corrige ese flag

Lectura practica:

- `reporting_configured` no garantiza que todos los atributos hayan aceptado el reporting
- significa mas bien "el coordinador ya intento configurar reporting para este dispositivo"

Esto es importante si se usa ese flag como criterio de salud funcional del nodo.

## 7. La entrevista se considera completa antes de validar respuestas de reporting

`interview_done()` se ejecuta inmediatamente despues de `rc_configure_device(dev)`.

Eso implica:

- el dispositivo pasa a `DEV_STATE_CONFIGURED` sin esperar a `REPORT_CFG_RSP`
- la entrevista mide exito por capacidad de descubrimiento, no por confirmacion completa de reporting

Es una decision valida si se entiende asi, pero conviene que quede explicito.

## 8. `POWER_DESC` es no fatal; `SIMPLE_DESC` parcial tambien

El sistema esta disenado para preferir conocimiento parcial antes que fracaso total.

Eso significa:

- un `POWER_DESC` fallido no aborta
- un `SIMPLE_DESC` fallido en un endpoint no impide avanzar con los demas

Ventaja:

- mayor tolerancia a dispositivos incompletos o poco finos

Coste:

- puede quedar un modelo local parcial sin que el estado final sea `FAILED`

## 9. Los bindings dependen de respuesta real del nodo

La binding table no se inventa ni se reconstruye indirectamente.

Solo se actualiza desde `Mgmt_Bind_rsp`.

Si el nodo no responde:

- el refresh se detiene
- el sistema conserva la informacion conocida anteriormente si estaba persistida

Esto es coherente con el enfoque conservador del proyecto.

## 10. Los atributos "unsupported" se recuerdan temporalmente

Cuando un `Read Attributes Response` devuelve `UNSUP_ATTRIB`:

- ese atributo se anota
- futuros sleepy probes lo evitan

Ademas, al reentrevistar el dispositivo:

- esos marcadores se limpian

Eso permite que un nuevo firmware o una nueva configuracion del nodo no quede bloqueada por un recuerdo viejo.

## 11. El sistema usa dos estilos de refresco tras trafico de un sleepy device

Si el sleepy device despierta y reporta:

- se aprovecha para pedir atributos faltantes

Si ya no faltan atributos pero no hay metricas:

- se fuerza una lectura minima para intentar obtener RSSI/LQI

Es un patron util: usar la ventana despierta del nodo para enriquecer el estado sin hacer polling agresivo constante.

## 12. El contador `interview_attempts` mezcla intentos y fallos

En el flujo actual:

- `start_interview()` incrementa `interview_attempts`
- `interview_fail()` vuelve a incrementarlo

Consecuencia:

- el valor no representa solo "numero de arranques de entrevista"
- tampoco representa solo "numero de entrevistas fallidas"
- es una mezcla de ambos

Para diagnostico humano puede seguir siendo util, pero semanticamente no es limpio.

## 13. La cola de entrevistas es pequena y descarta en saturacion

Solo hay:

- una entrevista activa
- una cola FIFO de tamano 8

Si entran demasiados dispositivos a la vez:

- el sistema puede logear `INTERVIEW queue full, dropping ...`

No es necesariamente un problema en despliegues pequenos, pero es un dato importante para comisionado masivo.

## 14. El endpoint usado para comandos es el primer endpoint que contenga el cluster

Para On/Off, Level o reads bajo demanda:

- se usa el primer endpoint encontrado con ese cluster

Esto funciona bien en muchos dispositivos simples, pero en dispositivos multi-endpoint implica:

- convencion operativa, no seleccion explicita del destino semantico

## 15. El proyecto depende fuertemente de los logs para entender estados

Hay una instrumentacion bastante buena en:

- `SIGNAL ...`
- `INTERVIEW ...`
- `REPORT_CFG ...`
- `RX RAW ...`
- `RX DECODE ...`
- `STATE ...`
- `BINDING_TABLE ...`

Eso facilita reconstruir el flujo real del dispositivo sin depender de herramientas externas.

## 16. Imagen mental recomendada del sistema

La mejor manera de entender este coordinador es pensar que cada dispositivo tiene tres capas de conocimiento:

1. Identidad persistida

- IEEE
- NWK actual conocida
- manufacturer, model
- endpoints y clusters

2. Estado operativo Zigbee

- entrevistado o no
- reporting intentado o no
- bindings conocidos o no

3. Vitalidad reciente

- online/offline
- `last_seen_ms`
- ultimas metricas de radio
- cache ZCL reciente

La mayoria de comportamientos aparentemente raros salen de mezclar estas tres capas.

## 17. Preguntas utiles para futuras revisiones

- Queremos que `DEVICE_UNAVAILABLE` implique `offline` inmediato o solo una sospecha temporal
- Queremos que `reporting_configured` signifique "enviado" o "confirmado"
- Queremos persistir parte de la cache ZCL tras reboot del coordinador
- Queremos reentrevistar mas agresivamente a dispositivos restaurados desde NVS
- Queremos distinguir mejor intents, confirms y observaciones en telemetria y estado

## 18. Criterio practico para leer incidentes

Cuando un dispositivo se comporte raro, conviene separar siempre:

- si el coordinador conoce bien su estructura
- si el coordinador lo esta viendo online justo ahora
- si el dispositivo esta reportando por si mismo
- si el coordinador solo esta sobreviviendo con datos persistidos

Ese marco suele aclarar muy rapido si el problema es:

- de descubrimiento
- de reporting
- de presencia
- de cache local
- o solo de falta de trafico reciente
