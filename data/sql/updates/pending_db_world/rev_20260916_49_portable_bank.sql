-- Ascension's summon-bank items cast a bare SPELL_EFFECT_DUMMY; bind the handler that opens the bank.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_ascension_summon_bank';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(92078, 'spell_ascension_summon_bank'),
(93416, 'spell_ascension_summon_bank'),
(100702, 'spell_ascension_summon_bank');
