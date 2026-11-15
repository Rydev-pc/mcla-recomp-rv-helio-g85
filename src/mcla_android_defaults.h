#ifndef MCLA_ANDROID_DEFAULTS_H
#define MCLA_ANDROID_DEFAULTS_H

// Aplica defaults de perfil para dispositivos Android de baixa memória
// (Helio G85 / Mali-G52 com 4 GB RAM), seguindo a estratégia do Skate 3 Mobile:
// detecção de RAM em tempo de execução e redução automática de orçamentos.
void MclaApplyAndroidDefaults();

#endif
