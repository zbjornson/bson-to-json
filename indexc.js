const bindings = require("./build/release/bsonToJson.node");

const fs = require("fs");
const data = fs.readFileSync("./data.bson");

let json;

// json = bindings.bsonToJson(data);

console.time("transcode");
for (let i = 0; i < 100; i++)
	json = bindings.bsonToJson(data);
console.timeEnd("transcode");
fs.writeFileSync("./datac.json", json);
require("./datac.json");
console.log("ok");
