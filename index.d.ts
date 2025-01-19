export class Transcoder {
	/**
	 * @param p Instance of PopulateInfo if populating paths.
	 */
	constructor(p?: PopulateInfo);
	/**
	 * Transcodes the BSON buffer `b` into a JSON string stored in a Buffer.
	 * @param b BSON buffer
	 */
	transcode(b: Uint8Array): Buffer;
}

export class PopulateInfo {
	/**
	 * Adds objects for a path.
	 * @param path The path to populate. Can be dotted.
	 * @param items BSON buffers to populate the path with.
	 */
	addItems(path: string, items: Buffer[]): void;
	/**
	 * Reuses objects for one path for another path.
	 * @param path1 The path that's already had `addItems()` called for it.
	 * @param path2 The path to reuse that path's items on.
	 */
	repeatPath(path1: string, path2: string): void;
}
