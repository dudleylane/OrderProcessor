// The WebSocket access token (#146). A server run with OMS_WS_TOKEN or --ws-token-file refuses a connection that doesn't
// offer it, and a browser can't set a WebSocket's headers, so the app offers the token as a second subprotocol.
// app/WsAuth.h holds the server's side of these rules.

/** The subprotocol the app speaks, which the server names in its answer */
export const WS_SUBPROTOCOL = 'oms-v1';
/** The token travels as a subprotocol: this prefix, then the token */
export const WS_TOKEN_PREFIX = 'oms-token.';
const MIN_TOKEN_LENGTH = 16;
const STORAGE_KEY = 'oms.accessToken';

/** Why a token can't be offered, or null when it can. Empty is for a server that asks for none. */
export function accessTokenProblem(token: string): string | null {
  if (token === '') {
    return null;
  }
  // The WebSocket constructor throws on a subprotocol with other characters
  if (!/^[A-Za-z0-9._-]+$/.test(token)) {
    return "A token has only letters, digits, '.', '_' and '-'.";
  }
  if (token.length < MIN_TOKEN_LENGTH) {
    return `A token has at least ${MIN_TOKEN_LENGTH} characters.`;
  }
  return null;
}

/** The subprotocols to offer: the app's, and the token's when there is one */
export function wsSubprotocols(token: string): string[] {
  return token === '' ? [WS_SUBPROTOCOL] : [WS_SUBPROTOCOL, WS_TOKEN_PREFIX + token];
}

// The token lasts as long as the tab. A browser may refuse sessionStorage; then the app asks again on the next load.
export function loadAccessToken(): string | null {
  try {
    return window.sessionStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

export function saveAccessToken(token: string | null): void {
  try {
    if (token === null) {
      window.sessionStorage.removeItem(STORAGE_KEY);
    } else {
      window.sessionStorage.setItem(STORAGE_KEY, token);
    }
  } catch {
    // Not kept: the next load asks again
  }
}
