import { EventEmitter } from 'node:events';
import WebSocket from 'ws';
import type { Logger } from 'homebridge';

import {
  CmdResultMessage,
  ErrorMessage,
  InventoryDevice,
  StateDeviceSnapshot,
  WS_PROTOCOL_VERSION,
  ZigbeeEventMessage,
} from './model';
import { resolveGatewayTarget } from './mdns';

interface BaseWsEnvelope<TData = unknown> {
  type: string;
  msg_id: number;
  reply_to?: number;
  ts?: number;
  require_ack?: boolean;
  data: TData;
}

interface InventoryChunkData {
  stream_id: string;
  generation: number;
  index: number;
  final: boolean;
  devices: InventoryDevice[];
}

interface StateChunkData {
  stream_id: string;
  generation: number;
  index: number;
  final: boolean;
  devices: StateDeviceSnapshot[];
}

interface StreamAccumulator<TItem> {
  streamId: string;
  generation: number;
  chunks: Map<number, TItem[]>;
  finalIndex?: number;
}

interface PendingCommand {
  timer: NodeJS.Timeout;
  resolve: (result: CmdResultMessage) => void;
  reject: (error: Error) => void;
}

export interface GatewayClientOptions {
  url: string;
  pingIntervalMs: number;
  reconnectDelayMs: number;
  commandTimeoutMs: number;
}

export declare interface GatewayClient {
  on(event: 'connected', listener: () => void): this;
  on(event: 'disconnected', listener: (reason?: string) => void): this;
  on(event: 'hello', listener: (payload: Record<string, unknown>) => void): this;
  on(event: 'inventory', listener: (devices: InventoryDevice[], generation: number) => void): this;
  on(event: 'state', listener: (devices: StateDeviceSnapshot[], generation: number) => void): this;
  on(event: 'event', listener: (messageType: string, payload: ZigbeeEventMessage) => void): this;
}

export class GatewayClient extends EventEmitter {
  private socket?: WebSocket;
  private readonly inventoryStream = new Map<string, StreamAccumulator<InventoryDevice>>();
  private readonly stateStream = new Map<string, StreamAccumulator<StateDeviceSnapshot>>();
  private readonly pendingCommands = new Map<number, PendingCommand>();
  private reconnectTimer?: NodeJS.Timeout;
  private pingTimer?: NodeJS.Timeout;
  private nextMsgId = 1;
  private intentionalStop = false;
  private connected = false;
  private connecting = false;

  constructor(
    private readonly logger: Logger,
    private readonly options: GatewayClientOptions,
  ) {
    super();
  }

  public start(): void {
    this.intentionalStop = false;
    void this.connect();
  }

  public stop(): void {
    this.intentionalStop = true;
    this.clearReconnectTimer();
    this.clearPingTimer();
    this.rejectAllPending('Gateway client stopped');
    this.socket?.removeAllListeners();
    this.socket?.close();
    this.socket = undefined;
    this.connected = false;
  }

  public isConnected(): boolean {
    return this.connected && this.socket?.readyState === WebSocket.OPEN;
  }

  public async setOnOff(deviceId: string, state: boolean): Promise<CmdResultMessage> {
    return this.sendCommand({
      type: 'cmd',
      msg_id: this.allocateMsgId(),
      require_ack: true,
      data: {
        device_id: deviceId,
        cluster: 'onoff',
        command: 'set',
        params: {
          state,
        },
      },
    });
  }

  public requestResync(): void {
    this.send({
      type: 'resync',
      msg_id: this.allocateMsgId(),
      require_ack: true,
      data: {},
    });
  }

  private async connect(): Promise<void> {
    if (this.intentionalStop || this.socket || this.connecting) {
      return;
    }

    this.connecting = true;
    this.logger.info(`Conectando al gateway WS ${this.options.url}`);

    let target;
    try {
      target = await resolveGatewayTarget(this.options.url);
    } catch (error) {
      this.connecting = false;
      this.logger.warn(`Error resolviendo gateway: ${(error as Error).message}`);
      this.scheduleReconnect();
      return;
    }

    if (this.intentionalStop || this.socket) {
      this.connecting = false;
      return;
    }

    if (target.resolvedAddress) {
      this.logger.info(
        `Hostname resuelto por ${target.resolutionSource}: ${this.options.url} -> ${target.resolvedAddress}`,
      );
    }

    const socket = new WebSocket(target.connectionUrl, {
      headers: target.hostHeader ? { Host: target.hostHeader } : undefined,
    });
    this.socket = socket;

    socket.on('open', () => {
      this.connecting = false;
      this.connected = true;
      this.logger.info('Gateway WS conectado');
      this.emit('connected');
      this.sendHello();
      this.startPingLoop();
    });

    socket.on('message', (data, isBinary) => {
      if (isBinary) {
        return;
      }
      this.handleMessage(data.toString());
    });

    socket.on('close', (code, reason) => {
      const closeReason = reason.toString() || `code=${code}`;
      this.handleDisconnect(closeReason);
    });

    socket.on('error', error => {
      this.connecting = false;
      this.logger.warn(`Error WebSocket: ${error.message}`);
    });
  }

  private handleDisconnect(reason?: string): void {
    this.connecting = false;
    this.clearPingTimer();
    this.rejectAllPending(reason ? `Gateway disconnected: ${reason}` : 'Gateway disconnected');

    if (this.connected) {
      this.logger.warn(`Gateway WS desconectado (${reason ?? 'sin detalle'})`);
    }

    this.connected = false;
    this.socket?.removeAllListeners();
    this.socket = undefined;
    this.emit('disconnected', reason);

    if (!this.intentionalStop) {
      this.scheduleReconnect();
    }
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer || this.intentionalStop) {
      return;
    }

    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = undefined;
      this.connect();
    }, this.options.reconnectDelayMs);
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = undefined;
    }
  }

  private startPingLoop(): void {
    this.clearPingTimer();
    this.pingTimer = setInterval(() => {
      if (!this.isConnected()) {
        return;
      }

      this.send({
        type: 'ping',
        msg_id: this.allocateMsgId(),
        require_ack: false,
        data: {},
      });
    }, this.options.pingIntervalMs);
  }

  private clearPingTimer(): void {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = undefined;
    }
  }

  private sendHello(): void {
    this.send({
      type: 'hello',
      msg_id: this.allocateMsgId(),
      require_ack: false,
      data: {
        protocol_version: WS_PROTOCOL_VERSION,
        client: 'homebridge-esp32-zigbee',
      },
    });
  }

  private send(payload: object): void {
    if (!this.isConnected() || !this.socket) {
      this.logger.debug('WS send omitido porque no hay conexion activa');
      return;
    }

    this.socket.send(JSON.stringify(payload));
  }

  private sendCommand(payload: BaseWsEnvelope<Record<string, unknown>>): Promise<CmdResultMessage> {
    if (!this.isConnected() || !this.socket) {
      return Promise.reject(new Error('Gateway is not connected'));
    }

    return new Promise<CmdResultMessage>((resolve, reject) => {
      const command = typeof payload.data.command === 'string' ? payload.data.command : 'unknown';
      const deviceId = typeof payload.data.device_id === 'string' ? payload.data.device_id : 'unknown';
      const timer = setTimeout(() => {
        this.pendingCommands.delete(payload.msg_id);
        this.logger.warn(`Timeout esperando cmd_result msg_id=${payload.msg_id} command=${command} device=${deviceId}`);
        reject(new Error(`Command timeout after ${this.options.commandTimeoutMs} ms`));
      }, this.options.commandTimeoutMs);

      this.pendingCommands.set(payload.msg_id, {
        timer,
        resolve,
        reject,
      });

      this.socket?.send(JSON.stringify(payload), error => {
        if (!error) {
          return;
        }

        const pending = this.pendingCommands.get(payload.msg_id);
        if (!pending) {
          return;
        }

        clearTimeout(pending.timer);
        this.pendingCommands.delete(payload.msg_id);
        pending.reject(error);
      });
    });
  }

  private handleMessage(raw: string): void {
    let message: BaseWsEnvelope;

    try {
      message = JSON.parse(raw) as BaseWsEnvelope;
    } catch (error) {
      this.logger.warn(`Mensaje WS invalido: ${(error as Error).message}`);
      return;
    }

    switch (message.type) {
      case 'hello_ack':
        this.emit('hello', (message.data as Record<string, unknown>) ?? {});
        break;
      case 'inventory_chunk':
        this.acceptInventoryChunk(message as BaseWsEnvelope<InventoryChunkData>);
        break;
      case 'state_chunk':
        this.acceptStateChunk(message as BaseWsEnvelope<StateChunkData>);
        break;
      case 'event':
      case 'device_updated':
      case 'device_joined':
      case 'device_left':
        this.emit('event', message.type, (message.data as ZigbeeEventMessage) ?? { device_id: null });
        break;
      case 'cmd_result':
        this.resolvePendingResult(message.reply_to, message.data as CmdResultMessage);
        break;
      case 'error':
        this.rejectPendingResult(message.reply_to, message.data as ErrorMessage);
        break;
      case 'ack':
      case 'pong':
        break;
      default:
        this.logger.debug(`Tipo WS no procesado: ${message.type}`);
        break;
    }
  }

  private acceptInventoryChunk(message: BaseWsEnvelope<InventoryChunkData>): void {
    this.acceptChunk(this.inventoryStream, message.data, (devices, generation) => {
      this.emit('inventory', devices, generation);
    });
  }

  private acceptStateChunk(message: BaseWsEnvelope<StateChunkData>): void {
    this.acceptChunk(this.stateStream, message.data, (devices, generation) => {
      this.emit('state', devices, generation);
    });
  }

  private acceptChunk<TItem>(
    streams: Map<string, StreamAccumulator<TItem>>,
    data: { stream_id: string; generation: number; index: number; final: boolean; devices: TItem[] },
    onComplete: (devices: TItem[], generation: number) => void,
  ): void {
    const existing = streams.get(data.stream_id) ?? {
      streamId: data.stream_id,
      generation: data.generation,
      chunks: new Map<number, TItem[]>(),
    };

    existing.generation = data.generation;
    existing.chunks.set(data.index, Array.isArray(data.devices) ? data.devices : []);

    if (data.final) {
      existing.finalIndex = data.index;
    }

    streams.set(data.stream_id, existing);

    if (existing.finalIndex === undefined) {
      return;
    }

    for (let index = 0; index <= existing.finalIndex; index++) {
      if (!existing.chunks.has(index)) {
        return;
      }
    }

    const items: TItem[] = [];
    for (let index = 0; index <= existing.finalIndex; index++) {
      const chunk = existing.chunks.get(index);
      if (chunk) {
        items.push(...chunk);
      }
    }

    streams.delete(data.stream_id);
    onComplete(items, existing.generation);
  }

  private resolvePendingResult(replyTo: number | undefined, result: CmdResultMessage): void {
    if (!replyTo) {
      return;
    }

    const pending = this.pendingCommands.get(replyTo);
    if (!pending) {
      return;
    }

    clearTimeout(pending.timer);
    this.pendingCommands.delete(replyTo);

    if (result.status === 'ok' && result.applied) {
      pending.resolve(result);
      return;
    }

    pending.reject(new Error(result.error || 'Command failed'));
  }

  private rejectPendingResult(replyTo: number | undefined, error: ErrorMessage): void {
    if (!replyTo) {
      return;
    }

    const pending = this.pendingCommands.get(replyTo);
    if (!pending) {
      return;
    }

    clearTimeout(pending.timer);
    this.pendingCommands.delete(replyTo);
    pending.reject(new Error(`${error.code}: ${error.message}`));
  }

  private rejectAllPending(message: string): void {
    for (const [msgId, pending] of this.pendingCommands) {
      clearTimeout(pending.timer);
      pending.reject(new Error(message));
      this.pendingCommands.delete(msgId);
    }
  }

  private allocateMsgId(): number {
    const current = this.nextMsgId;
    this.nextMsgId += 1;
    if (this.nextMsgId > Number.MAX_SAFE_INTEGER) {
      this.nextMsgId = 1;
    }
    return current;
  }
}
