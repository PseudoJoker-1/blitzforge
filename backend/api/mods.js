import { loadRegistry, approved, publicView, send } from './_registry.js';

export default async function handler(req, res) {
  if (req.method !== 'GET') return send(res, 405, { error: 'method_not_allowed' });

  const registry = await loadRegistry();
  let mods = approved(registry);

  const { type, q } = req.query ?? {};
  if (type) mods = mods.filter((mod) => mod.type === type);
  if (q) {
    const needle = String(q).toLowerCase();
    mods = mods.filter(
      (mod) =>
        mod.name.toLowerCase().includes(needle) ||
        mod.description.toLowerCase().includes(needle) ||
        mod.author.toLowerCase().includes(needle)
    );
  }

  send(res, 200, {
    schema: registry.schema,
    updated: registry.updated,
    count: mods.length,
    mods: mods.map(publicView),
  });
}
