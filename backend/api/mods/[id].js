import { loadRegistry, approved, publicView, send } from '../_registry.js';

export default async function handler(req, res) {
  if (req.method !== 'GET') return send(res, 405, { error: 'method_not_allowed' });

  const registry = await loadRegistry();
  const mod = approved(registry).find((candidate) => candidate.id === req.query.id);

  // An unapproved mod is reported as absent rather than rejected: telling a
  // caller that a hidden id exists is still leaking the pending queue.
  if (!mod) return send(res, 404, { error: 'not_found' });

  send(res, 200, publicView(mod));
}
