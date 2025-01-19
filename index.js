try {
	module.exports = require("./build/Release/bsonToJson.node");
} catch (ex) {
	module.exports = require("./src/bson-to-json.js");
}

const {Transcoder} = module.exports;

const C_OPEN_SQ = Buffer.from("[");
const C_COMMA = Buffer.from(",");
const C_CLOSE_SQ = Buffer.from("]");

/**
 * Writes the entire cursor to `ostr`, closing `ostr` when done.
 * *This particular implementation is not recommended.*
 * @param {import("mongodb").Cursor} cursor
 * @param {import("stream").Writable} ostr
 */
async function send(cursor, ostr) {
	cursor.rewind();

	if (cursor.isDead())
		throw new Error("Cursor is closed.");

	ostr.write(C_OPEN_SQ);

	const t = new Transcoder();
	let rest = false;
	while (true) {
		// Read all buffered documents. This loop doesn't wait for the stream to
		// drain: The source documents are already in memory, so try to free
		// that memory up ASAP (although it allocates new memory).
		const {cursorState} = cursor;
		const documents = cursorState.documents;
		let i = cursorState.cursorIndex;
		while (i < documents.length - 1) {
			const doc = documents[i++];
			if (!doc)
				break;
			ostr.write(t.transcode(doc));
			ostr.write(C_COMMA);
		}
		const lastDoc = documents[i];
		if (lastDoc) {
			const shouldWaitDrain = !ostr.write(t.transcode(lastDoc));
			cursorState.cursorIndex = documents.length;
			if (shouldWaitDrain)
				await new Promise(resolve => ostr.once("drain", resolve));
		}

		// Get next batch from MongoDB (i.e. issue GET_MORE).
		const next = await cursor.next();
		if (!next)
			break;
		// Rewind: will deal with this document in next iteration.
		cursorState.cursorIndex--;

		if (rest)
			ostr.write(C_COMMA);
		else
			rest = true;
	}

	ostr.end(C_CLOSE_SQ);
}

module.exports.send = send;
