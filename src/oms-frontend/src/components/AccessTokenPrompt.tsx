import React, { useState } from 'react';
import { accessTokenProblem } from '../utils/accessToken';

interface AccessTokenPromptProps {
  onSubmit: (token: string) => void;
}

/** Asks for the server's access token before the app connects (#146) */
export function AccessTokenPrompt({ onSubmit }: AccessTokenPromptProps) {
  const [token, setToken] = useState('');
  const trimmed = token.trim();
  const problem = accessTokenProblem(trimmed);

  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    if (problem === null) {
      onSubmit(trimmed);
    }
  };

  return (
    <div className="min-h-screen bg-gray-950 text-gray-100 flex items-center justify-center px-4">
      <form onSubmit={handleSubmit} className="w-full max-w-sm space-y-4 border border-gray-800 rounded-lg p-6">
        <h1 className="text-xl font-semibold tracking-tight">
          OrderProcessor <span className="text-blue-400">OMS</span>
        </h1>
        <div>
          <label htmlFor="access-token" className="block text-xs text-gray-500 mb-1">Access token</label>
          <input
            id="access-token"
            type="password"
            autoComplete="off"
            autoFocus
            value={token}
            onChange={e => setToken(e.target.value)}
            className="w-full bg-gray-900 border border-gray-700 rounded px-2 py-1.5 text-sm font-mono"
          />
          <div className="text-xs text-gray-600 mt-1">
            The server's OMS_WS_TOKEN. Leave it empty if the server runs without one.
          </div>
          {problem && (
            <div className="text-xs text-red-400 mt-1">{problem}</div>
          )}
        </div>
        <button
          type="submit"
          disabled={problem !== null}
          className="w-full py-2.5 rounded font-medium text-sm transition-colors
            bg-blue-600 hover:bg-blue-500 disabled:bg-gray-700 disabled:text-gray-500"
        >
          Connect
        </button>
      </form>
    </div>
  );
}
