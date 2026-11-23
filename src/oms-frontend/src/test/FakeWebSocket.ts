/**
 * A stand-in for the browser's WebSocket, for the hook's tests (#548). A test opens it, feeds it the server's messages,
 * drops it and reads what the hook sent. As a browser's does, it throws on a send before it opens (#522), and close()
 * fires the close event later, as a task of its own (#523), so a test moves the timers on to see it.
 */
export class FakeWebSocket {
  static readonly CONNECTING = 0;
  static readonly OPEN = 1;
  static readonly CLOSING = 2;
  static readonly CLOSED = 3;
  /** Every socket made since reset(), oldest first */
  static instances: FakeWebSocket[] = [];

  readonly url: string;
  readonly protocols: string[];
  readyState: number = FakeWebSocket.CONNECTING;
  readonly sent: string[] = [];
  onopen: ((ev: Event) => void) | null = null;
  onclose: ((ev: CloseEvent) => void) | null = null;
  onerror: ((ev: Event) => void) | null = null;
  onmessage: ((ev: MessageEvent) => void) | null = null;

  constructor(url: string, protocols?: string | string[]) {
    this.url = url;
    this.protocols = protocols === undefined ? [] : Array.isArray(protocols) ? protocols : [protocols];
    FakeWebSocket.instances.push(this);
  }

  static reset(): void {
    FakeWebSocket.instances = [];
  }

  /** The newest socket */
  static latest(): FakeWebSocket {
    const socket = FakeWebSocket.instances[FakeWebSocket.instances.length - 1];
    if (!socket) {
      throw new Error('no socket was made');
    }
    return socket;
  }

  send(data: string): void {
    if (this.readyState !== FakeWebSocket.OPEN) {
      throw new DOMException('send() before the socket is open', 'InvalidStateError');
    }
    this.sent.push(data);
  }

  close(): void {
    if (this.readyState === FakeWebSocket.CLOSING || this.readyState === FakeWebSocket.CLOSED) {
      return;
    }
    this.readyState = FakeWebSocket.CLOSING;
    setTimeout(() => {
      this.readyState = FakeWebSocket.CLOSED;
      this.onclose?.(new CloseEvent('close'));
    }, 0);
  }

  // What a test does to the socket

  /** The server takes the connection */
  open(): void {
    this.readyState = FakeWebSocket.OPEN;
    this.onopen?.(new Event('open'));
  }

  /** A message from the server */
  receive(message: unknown): void {
    this.onmessage?.(new MessageEvent('message', { data: JSON.stringify(message) }));
  }

  /** The connection ends without the hook closing it, as when the server stops */
  drop(): void {
    this.readyState = FakeWebSocket.CLOSED;
    this.onclose?.(new CloseEvent('close'));
  }

  /** What the hook sent, each message parsed */
  messages(): unknown[] {
    return this.sent.map(s => JSON.parse(s) as unknown);
  }
}
