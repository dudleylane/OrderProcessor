import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

// The DOM a test rendered into is emptied after it, as Testing Library does by itself only where the runner defines
// afterEach globally, which this config doesn't
afterEach(() => {
  cleanup();
});
