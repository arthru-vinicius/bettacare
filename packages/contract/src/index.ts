/**
 * @bettacare/contract — a fonte única do contrato.
 *
 * O corpo do POST é definido **uma vez**, aqui. O servidor valida com estes
 * schemas, a interface tipa com eles, e o `.h` do firmware é gerado a partir
 * deles. No sistema antigo o formato do JSON estava escrito à mão em três
 * lugares (`web_server.cpp`, `mqtt_manager.cpp`, `app.html`) e os três já
 * divergiam entre si.
 */
export * from "./primitives.js";
export * from "./codes.js";
export * from "./events.js";
export * from "./health.js";
export * from "./commands.js";
export * from "./config.js";
export * from "./telemetry.js";
export * from "./api.js";
