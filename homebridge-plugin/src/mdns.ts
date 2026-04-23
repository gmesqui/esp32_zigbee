import { promises as dns } from 'node:dns';
import { URL } from 'node:url';

const createMdns = require('multicast-dns') as () => MdnsClient;

interface MdnsRecord {
  name?: string;
  type?: string;
  data?: unknown;
}

interface MdnsResponse {
  answers?: MdnsRecord[];
  additionals?: MdnsRecord[];
}

interface MdnsQuestion {
  name: string;
  type: 'A' | 'AAAA';
}

interface MdnsClient {
  on(event: 'response', listener: (response: MdnsResponse) => void): void;
  removeListener(event: 'response', listener: (response: MdnsResponse) => void): void;
  query(packet: { questions: MdnsQuestion[] }): void;
  destroy(): void;
}

export interface ResolvedGatewayTarget {
  connectionUrl: string;
  hostHeader?: string;
  resolvedAddress?: string;
  resolutionSource?: 'dns' | 'mdns';
}

const MDNS_TIMEOUT_MS = 1500;

export async function resolveGatewayTarget(rawUrl: string): Promise<ResolvedGatewayTarget> {
  const url = new URL(rawUrl);
  const hostname = url.hostname.trim();
  if (!hostname.endsWith('.local')) {
    return {
      connectionUrl: rawUrl,
    };
  }

  const hostHeader = url.host;
  const dnsResult = await resolveWithDns(hostname);
  if (dnsResult) {
    url.hostname = dnsResult;
    return {
      connectionUrl: url.toString(),
      hostHeader,
      resolvedAddress: dnsResult,
      resolutionSource: 'dns',
    };
  }

  const mdnsResult = await resolveWithMdns(hostname);
  if (mdnsResult) {
    url.hostname = mdnsResult;
    return {
      connectionUrl: url.toString(),
      hostHeader,
      resolvedAddress: mdnsResult,
      resolutionSource: 'mdns',
    };
  }

  throw new Error(`No se pudo resolver ${hostname} por DNS ni por mDNS`);
}

async function resolveWithDns(hostname: string): Promise<string | undefined> {
  try {
    const result = await dns.lookup(hostname);
    return result.address;
  } catch {
    return undefined;
  }
}

async function resolveWithMdns(hostname: string): Promise<string | undefined> {
  return new Promise<string | undefined>((resolve) => {
    const mdns = createMdns();
    const expectedName = normalizeHostname(hostname);

    const cleanup = () => {
      clearTimeout(timer);
      mdns.removeListener('response', onResponse);
      mdns.destroy();
    };

    const finish = (value: string | undefined) => {
      cleanup();
      resolve(value);
    };

    const onResponse = (response: MdnsResponse) => {
      const records = [...(response.answers ?? []), ...(response.additionals ?? [])];
      for (const record of records) {
        if (normalizeHostname(record.name) !== expectedName) {
          continue;
        }

        if ((record.type === 'A' || record.type === 'AAAA') && typeof record.data === 'string') {
          finish(record.data);
          return;
        }
      }
    };

    const timer = setTimeout(() => {
      finish(undefined);
    }, MDNS_TIMEOUT_MS);

    mdns.on('response', onResponse);
    mdns.query({
      questions: [
        { name: hostname, type: 'A' },
        { name: hostname, type: 'AAAA' },
      ],
    });
  });
}

function normalizeHostname(value: string | undefined): string {
  return (value ?? '').trim().replace(/\.$/, '').toLowerCase();
}
