/** @type {import('next').NextConfig} */

// Content Security Policy. The gallery renders images that guests upload, so the
// image sources are deliberately wider than everything else, which stays locked to
// this origin. No inline script is allowed beyond Next's own hydration bootstrap.
// Vercel always serves over https. A local `next start` does not, and it also
// runs with NODE_ENV=production, so NODE_ENV cannot be the signal here.
const isHttpsHost = Boolean(process.env.VERCEL);

const csp = [
  "default-src 'self'",
  "script-src 'self' 'unsafe-inline'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' blob: data: https://*.public.blob.vercel-storage.com",
  "font-src 'self' data:",
  "connect-src 'self' https://*.public.blob.vercel-storage.com https://blob.vercel-storage.com",
  "media-src 'self' blob:",
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'self'",
  "frame-ancestors 'none'",
  // Only where the host is https. Over plain http on localhost this upgrades every
  // subresource to https, which aborts the Next.js chunks and stops the page
  // hydrating during local testing.
  ...(isHttpsHost ? ["upgrade-insecure-requests"] : []),
].join("; ");

const nextConfig = {
  reactStrictMode: true,
  poweredByHeader: false,
  images: {
    remotePatterns: [
      { protocol: "https", hostname: "*.public.blob.vercel-storage.com" },
    ],
  },
  async headers() {
    return [
      {
        source: "/:path*",
        headers: [
          { key: "Content-Security-Policy", value: csp },
          { key: "X-Content-Type-Options", value: "nosniff" },
          { key: "X-Frame-Options", value: "DENY" },
          { key: "Referrer-Policy", value: "strict-origin-when-cross-origin" },
          { key: "Permissions-Policy", value: "camera=(self), microphone=(), geolocation=()" },
          { key: "Strict-Transport-Security", value: "max-age=63072000; includeSubDomains; preload" },
        ],
      },
    ];
  },
};

module.exports = nextConfig;
