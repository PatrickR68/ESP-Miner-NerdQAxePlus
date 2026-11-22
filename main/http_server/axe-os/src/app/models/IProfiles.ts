export interface IProfileFanPid {
    targetTemp: number;
    p: number;
    i: number;
    d: number;
}

export interface IProfileFan {
    mode: number;
    manualSpeed: number;
    pid: IProfileFanPid;
}

// One profile slot as returned by GET /api/v2/profiles.
// Empty slots only carry id and complete = false.
export interface IProfile {
    id: number;
    complete: boolean;
    name?: string;
    frequency?: number;
    coreVoltage?: number;
    poolMode?: number; // 0 = failover, 1 = dual pool
    poolBalance?: number; // only has an effect in dual pool mode
    fans?: IProfileFan[];
}

export interface IProfiles {
    active: number; // id of the profile activated last, 0 = none
    profiles: IProfile[];
}

// Body of PUT /api/v2/profiles/<id>, all values are required
export interface IProfileSave {
    name: string;
    frequency: number;
    coreVoltage: number;
    poolMode: number;
    poolBalance: number;
    fans: IProfileFan[];
    activate?: boolean; // also activate the profile in the same request
    autoRestart?: boolean; // false: do not restart automatically if the pool mode changes (the UI offers a button)
}

// Answer of POST /api/system/set-profile
export interface IProfileActivateResult {
    active: number;
    restartRequired: boolean; // the pool mode changed, the device has to be restarted
    restarting?: boolean; // the device restarts by itself (autoRestart was not switched off)
}

// Answer of PUT /api/v2/profiles/<id>
export interface IProfileSaveResult {
    id: number;
    saved: boolean;
    activated?: boolean; // only if "activate" was requested and the firmware supports it
    restartRequired?: boolean;
    restarting?: boolean;
}
