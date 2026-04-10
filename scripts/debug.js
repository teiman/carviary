const { execSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const quakeDir = "d:\\quake";
const logFile = path.join(quakeDir, "carviary", "carviary.log");

if (fs.existsSync(logFile)) fs.unlinkSync(logFile);

console.log("Launching carviary: map start ...\n");

let exitCode = 0;
try {
  execSync(".\\carviary.exe -condebug +map start", {
    cwd: quakeDir,
    stdio: "inherit",
  });
} catch (e) {
  exitCode = e.status || 1;
}

console.log("\n--- carviary.log ---");
if (fs.existsSync(logFile)) {
  const log = fs.readFileSync(logFile, "utf8");
  console.log(log);

  const errors = log.split("\n").filter((l) => /ERROR|WARNING|Exception/i.test(l));
  if (errors.length) {
    console.log("--- errors/warnings ---");
    errors.forEach((e) => console.log(e));
  }
} else {
  console.log("(no log file generated)");
}

console.log(`\nExit code: ${exitCode}`);
process.exit(exitCode);
