---
id: localization
title: Langues et messages
description: Guide : Langues et messages.
section: Premiers pas
order: 170
sourceRevision: 1
---

# Langue et localisation {#language-and-localization-policy}

L’anglais est la langue source et la langue d’interface par défaut de Syrnck ; le français est intégré. La sélection se fait par session avec repli anglais.

## Sélection en ligne de commande {#command-line-selection}

`--lang` prime sur `SYRNCK_LANG`. Sans les deux, l’anglais s’applique indépendamment de `LANG`, `LC_ALL` ou de la région Windows. `en` et `fr` acceptent aussi `en-US` et `fr_FR`. Une valeur explicite non reconnue échoue. La sortie est UTF-8.

Aide, états, bannière d’écoute et diagnostics appartenant à Syrnck sont traduits. Le catalogue compilé ne demande aucun fichier de langue installé et ne modifie ni langue globale mutable ni locale du processus.

```text
syrnck --help
syrnck --lang fr --help
syrnck --lang en --store data fsck
```


## Frontières bibliothèque et protocole {#library-and-protocol-boundaries}

Le cœur conserve les exceptions canoniques anglaises. Chaque appelant utilise `localize` pour l’affichage, avec `Language::english` par défaut ; plusieurs langues peuvent coexister. Les messages inconnus du système, de la bibliothèque standard ou des dépendances gardent leur langue d’origine.

Octets stockés, chemins, messages utilisateur, commandes/options, références, clés JSON, types, IDs, hashes, pointeurs Git et champs de protocole restent stables. Les diagnostics C/Go et HTTP sont canoniques anglais ; la présentation de l’application appelante choisit sa langue. Cela préserve automatisation, hashes et interopérabilité.

```cpp
#include <syrnck/i18n.hpp>

auto language = syrnck::parse_language("fr");
auto display = syrnck::localize("object hash mismatch", language);
```


## Langues du manuel {#maintaining-translations}

Le manuel local possède deux éditions complètes avec les mêmes IDs de pages et d’ancres. L’aide contextuelle reprend la langue du CLI ; le lecteur peut ensuite la changer indépendamment. Une route indisponible revient à l’anglais. Chaque traduction indique la révision anglaise couverte. Les instructions de maintenance sont conservées hors de ce manuel.
