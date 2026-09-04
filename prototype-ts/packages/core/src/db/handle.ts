/**
 * Object handles.
 *
 * Every object in a DWG database carries a permanent 64-bit handle, written in
 * the file as uppercase hexadecimal. Handles are how objects reference one
 * another — an entity names its layer by handle, a layout names its block
 * record by handle, an xref names its host — and they survive save/reload.
 *
 * This system uses handles as the sole object identity, deliberately:
 *
 *  - Direct object references would break as soon as undo replaces an object
 *    with a restored clone, leaving dangling pointers into old versions.
 *  - Array indices are invalidated by every deletion.
 *  - Handles round-trip to DWG exactly, so a reference we could not fully
 *    interpret on load can still be written back out intact.
 *
 * The branded string type prevents an arbitrary string from being used where a
 * handle is expected, without any runtime cost.
 */

declare const HandleBrand: unique symbol;

/** A permanent object identifier, as uppercase hex. */
export type Handle = string & { readonly [HandleBrand]: true };

/** The null handle, used where DWG stores a reference to nothing. */
export const NULL_HANDLE = '0' as Handle;

/**
 * Convert a numeric or string value to a canonical handle.
 * @throws {TypeError} when the text is not valid hexadecimal.
 */
export function toHandle(value: string | number): Handle {
  if (typeof value === 'number') {
    if (!Number.isInteger(value) || value < 0) {
      throw new TypeError(`Handle must be a non-negative integer, received ${value}`);
    }
    return value.toString(16).toUpperCase() as Handle;
  }
  const trimmed = value.trim().toUpperCase();
  if (trimmed === '' || !/^[0-9A-F]+$/.test(trimmed)) {
    throw new TypeError(`Invalid handle: ${JSON.stringify(value)}`);
  }
  // Strip leading zeros so that "0A" and "A" are the same handle.
  const canonical = trimmed.replace(/^0+(?=.)/, '');
  return canonical as Handle;
}

/** Parse a handle without throwing; returns `null` for malformed input. */
export function tryToHandle(value: string | number | null | undefined): Handle | null {
  if (value === null || value === undefined) return null;
  try {
    return toHandle(value);
  } catch {
    return null;
  }
}

export function isNullHandle(handle: Handle | null | undefined): boolean {
  return handle === null || handle === undefined || handle === NULL_HANDLE;
}

/** Numeric value of a handle, for ordering and for allocating the next one. */
export function handleToNumber(handle: Handle): number {
  return Number.parseInt(handle, 16);
}

/**
 * Allocates fresh handles that do not collide with any already in a database.
 *
 * DWG stores a "next handle" seed in its header; on load we take the larger of
 * that seed and one past the highest handle actually seen, so that a file with
 * an inconsistent seed still cannot produce a collision.
 */
export class HandleAllocator {
  private next: number;

  constructor(seed = 1) {
    this.next = Math.max(1, Math.floor(seed));
  }

  /** Allocate the next unused handle. */
  allocate(): Handle {
    const handle = this.next.toString(16).toUpperCase() as Handle;
    this.next++;
    return handle;
  }

  /**
   * Ensure future allocations come after `handle`. Called for every object
   * read from a file.
   */
  observe(handle: Handle): void {
    const value = handleToNumber(handle);
    if (Number.isFinite(value) && value >= this.next) {
      this.next = value + 1;
    }
  }

  /** The handle that will be allocated next, for writing the DWG header seed. */
  peek(): Handle {
    return this.next.toString(16).toUpperCase() as Handle;
  }

  get nextValue(): number {
    return this.next;
  }
}
