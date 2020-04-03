try {
	module.exports = require("./build/Release/bsonToJson.node");
} catch (ex) {
	module.exports = require("./src/bson-to-json.js");
}
