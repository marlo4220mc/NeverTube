#ifndef NT_LIVE_DETECT_H
#define NT_LIVE_DETECT_H

#include <QtGlobal>
#include <QDateTime>

namespace nt {

// Detección de "directo" (medio sin duración final) para la barra del
// reproductor (overlay y mini player, un detector por superficie). Un
// directo de YouTube no tiene duración final: mpv reporta 0 (HLS live puro)
// o la duración "hasta ahora", que crece con cada segmento entrante y nunca
// se queda quieta. Un VOD materializa su duración en el arranque (ningún
// cambio o uno solo) y luego la mantiene CONSTANTE, así que la duración que
// SOLO CRECE es la señal y el ESTADO se decide con evidencia, no con un solo
// tick:
//
//   - duración que CRECE más de kChangeS entre ticks (dur>0 en ambos) DOS
//     veces (kLockChanges): un VOD no crece dos veces; un directo crece con
//     cada segmento, indefinidamente. Se cuentan solo los ASCENSOS: un
//     descenso o un colapso de duración (un VOD en reload reinicia la suya:
//     p. ej. 17659.5 -> 0 -> 1213.6 — es el MISMO vídeo finito volviendo a
//     materializarse) nunca es señal de directo. Tampoco se usa la rama
//     "dur=0 con posición avanzada" que marcaba vídeos normales de arranque
//     lento: es imposible que un VOD llegue a 2 ascensos, así que ningún
//     vídeo normal se marca.
//
// Desbloqueo automático (red de seguridad): si en modo directo llega una
// duración ESTABLE (>0, sin ascensos significativos durante kStableUnlockMs),
// se vuelve a la barra normal. Un directo real SIGUE creciendo la duración
// con los segmentos, así que nunca se desbloquea solo; un vídeo marcado por
// error se libera a los ~15 s y recupera el seek.
//
// Nota: el HLS sintético de AV1 (fMP4 finito con playlist VOD+ENDLIST) NO se
// alimenta aquí — por construcción no es un directo y el backend le reporta
// duraciones inestables (ascensos y resets) mientras carga los rangos
// BYTERANGE. La superficie lo excluye con currentCanBeLive()==false.
class LiveDetector {
public:
    static constexpr double kChangeS = 0.5;
    static constexpr int kLockChanges = 2;
    static constexpr qint64 kStableUnlockMs = 15000;

    // Nuevo vídeo: la detección parte de cero (la duración se recibe de nuevo y
    // no se arrastra el estado del vídeo anterior).
    void reset()
    {
        m_changes = 0;
        m_live = false;
        m_lastDur = -1.0;
        m_lastChangeMs = 0;
    }

    bool isLive() const { return m_live; }

    // Entrada INMEDIATA por señal autoritativa (videoDetails.isLive del player
    // response de YouTube, cuando la superficie la conoce). Evita esperar los
    // kLockChanges ascensos de duración, y cubre los directos PUROS (dur=0
    // constante), que por definición nunca producen ascensos. La salida sigue
    // gobernada por el desbloqueo por estabilidad: el reloj del último cambio
    // se arma desde AHORA, así un directo que termina (la duración se fija y
    // deja de crecer) vuelve solo a la barra normal igual que sin esta función.
    void forceLive()
    {
        m_live = true;
        m_lastChangeMs = nowMs();
    }

    // Alimentar con cada tick de posición (~1/s). Devuelve el estado del medio
    // (true = directo) tras evaluar el tick.
    bool feed(double duration)
    {
        const qint64 now = nowMs();
        const double prevDur = m_lastDur;
        if (duration > 0)
            m_lastDur = duration;

        if (m_live) {
            // Ya en directo: refrescar el reloj del último cambio y desbloquear
            // si la duración se estabiliza (falso positivo -> barra normal).
            if (prevDur > 0 && duration > prevDur + kChangeS) {
                m_lastChangeMs = now;
            } else if (duration > 0 && now - m_lastChangeMs >= kStableUnlockMs) {
                m_live = false;
            }
            return m_live;
        }

        // Solo cuentan los ASCENSOS de duración. Un descenso no es señal: un
        // VOD en reload colapsa/reinicia la suya (17659.5 -> 0 -> 1213.6) y un
        // directo nunca baja — solo la hace crecer cada segmento.
        if (prevDur > 0 && duration > prevDur + kChangeS) {
            ++m_changes;
            m_lastChangeMs = now;
            if (m_changes >= kLockChanges)
                m_live = true;
        }
        return m_live;
    }

private:
    static qint64 nowMs()
    {
        static const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        return QDateTime::currentMSecsSinceEpoch() - t0;
    }

    int m_changes = 0;
    bool m_live = false;
    double m_lastDur = -1.0;
    qint64 m_lastChangeMs = 0;
};

} // namespace nt

#endif // NT_LIVE_DETECT_H