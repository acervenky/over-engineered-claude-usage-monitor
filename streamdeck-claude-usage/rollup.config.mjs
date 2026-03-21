import commonjs from "@rollup/plugin-commonjs";
import nodeResolve from "@rollup/plugin-node-resolve";
import typescript from "@rollup/plugin-typescript";
import path from "node:path";
import url from "node:url";

const isWatching = !!process.env.ROLLUP_WATCH;
const sdPlugin = "com.venky.claudeusage.sdPlugin";

export default {
  input: "src/plugin.ts",
  output: {
    file: `${sdPlugin}/bin/plugin.js`,
    format: "cjs",
    sourcemap: isWatching,
    sourcemapPathTransform: (relativeSourcePath) => {
      return url.pathToFileURL(
        path.resolve(path.dirname(`${sdPlugin}/bin/plugin.js`), relativeSourcePath)
      ).href;
    },
  },
  plugins: [
    typescript({ sourceMap: isWatching }),
    nodeResolve({ browser: false }),
    commonjs(),
  ],
};
