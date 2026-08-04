import type { Config } from "tailwindcss";

const config: Config = {
  content: ["./app/**/*.{ts,tsx}", "./components/**/*.{ts,tsx}", "./lib/**/*.{ts,tsx}"],
  theme: {
    extend: {
      colors: {
        ink: {
          900: "#0d0a14",
          800: "#151021",
          700: "#1d1730",
          600: "#282040",
        },
        gold: {
          400: "#f5c451",
          500: "#e0a92e",
          600: "#c08a1c",
        },
        ember: "#e2703a",
        cream: "#f6efe2",
        muted: "#a99fbe",
      },
      fontFamily: {
        display: ["var(--font-display)", "Georgia", "serif"],
        body: ["var(--font-body)", "system-ui", "sans-serif"],
      },
      screens: {
        xs: "420px",
      },
    },
  },
  plugins: [],
};

export default config;
