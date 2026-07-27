import registry from '../registry/mods.js';

// Files under api/ whose name starts with "_" are not routed by Vercel, so
// this module is shared code rather than an endpoint.

export async function loadRegistry() {
  return registry;
}

/*
 * The catalog in the game shows every mod the registry returns, so a mod that
 * has not cleared review must never reach this list. Filtering here rather
 * than in each endpoint means a new endpoint cannot forget to do it.
 */
export function approved(registry) {
  return registry.mods.filter((mod) => mod.review?.status === 'approved');
}

export function publicView(mod) {
  return {
    id: mod.id,
    name: mod.name,
    version: mod.version,
    author: mod.author,
    description: mod.description,
    long: mod.long,
    type: mod.type,
    downloads: mod.downloads,
    updated: mod.updated,
    artifact: mod.artifact,
  };
}

export function send(res, status, body) {
  res.setHeader('Content-Type', 'application/json; charset=utf-8');
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Cache-Control', 'public, max-age=60, s-maxage=300');
  res.status(status).send(JSON.stringify(body, null, 2));
}
