/*
 * The mod registry.
 *
 * Kept as a module rather than a JSON file on disk for two reasons: the
 * bundler follows a static import for certain, and nothing under this
 * directory is ever served as a static asset. That matters because this file
 * holds pending and rejected entries too — serving it raw would hand out the
 * review queue and bypass the approved-only filter completely.
 */
export default {
  "schema": 1,
  "updated": "2026-07-27",
  "mods": [
    {
      "id": "night-mode",
      "name": "Ночной режим",
      "version": "1.2.0",
      "author": "pseud",
      "description": "Ночная цветокоррекция сцены боя",
      "long": "Ночная цветокоррекция боевой сцены. Затемняет и обесцвечивает мир, сохраняя относительный контраст: ничто не становится заметнее, чем в оригинале.\nРаздельное тонирование теней и светов, S-кривая по яркости.\nЗатрагивает ландшафт, технику, небо, воду и растительность. Интерфейс не трогает.",
      "type": "resource",
      "downloads": 1240,
      "updated": "2026-07-27",
      "review": {
        "status": "approved",
        "reviewer": "pseud",
        "note": "Grade is applied uniformly to every surface, so nothing becomes easier to see than vanilla."
      },
      "artifact": null
    },
    {
      "id": "wet-surfaces",
      "name": "Мокрые поверхности",
      "version": "0.9.0",
      "author": "pseud",
      "description": "Намокание техники, земли и построек",
      "long": "Включает штатную ветку намокания движка, которая в игре скомпилирована выключенной.\nПористые материалы темнеют сильнее металла, шероховатость падает - появляется мокрый блик.",
      "type": "resource",
      "downloads": 318,
      "updated": "2026-07-27",
      "review": {
        "status": "approved",
        "reviewer": "pseud",
        "note": "Purely a material response change; no visibility advantage."
      },
      "artifact": null
    },
    {
      "id": "screen-rain",
      "name": "Экранный дождь",
      "version": "0.4.0",
      "author": "pseud",
      "description": "Полноэкранные капли поверх кадра",
      "long": "Полноэкранный проход дождя, который рисуется поверх готового кадра.\nСобран и работает, но выключен: капли живут в экранных координатах и не знают о глубине сцены, поэтому не взаимодействуют с объектами и не мокнут танки.",
      "type": "native",
      "downloads": 87,
      "updated": "2026-07-27",
      "review": {
        "status": "approved",
        "reviewer": "pseud",
        "note": "Draws over the finished frame only; reads no game state."
      },
      "artifact": null
    }
  ]
};
