const { execSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const quakeDir = "d:\\quake";
const carviaryDir = path.join(quakeDir, "carviary");
const profileSrc = path.join(carviaryDir, "profile.json");
const dataDir = path.join(carviaryDir, "data");

console.log("Running carviary profile...\n");

try {
  execSync(".\\carviary_sdl3.exe +vid_vsync 0 +profile demo1", {
    cwd: quakeDir,
    stdio: "inherit",
  });
} catch (e) {
  // engine may exit with non-zero, that's ok
}

if (fs.existsSync(profileSrc)) {
  fs.mkdirSync(dataDir, { recursive: true });
  const ts = new Date().toISOString().replace(/[T:]/g, "-").slice(0, 19);
  const dest = path.join(dataDir, `profile_${ts}.json`);
  fs.renameSync(profileSrc, dest);
  console.log(`Saved: ${dest}`);
  console.log(fs.readFileSync(dest, "utf8"));
} else {
  console.error("ERROR: profile.json was not generated");
  process.exit(1);
}
