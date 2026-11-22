import { HttpErrorResponse } from '@angular/common/http';
import { Component, EventEmitter, Input, OnDestroy, OnInit, Output } from '@angular/core';
import { NbToastrService } from '@nebular/theme';
import { TranslateService } from '@ngx-translate/core';
import { Subscription, of } from 'rxjs';
import { catchError, map, switchMap, tap } from 'rxjs/operators';
import { IProfile, IProfileActivateResult, IProfileFan, IProfileSave, IProfileSaveResult } from '../../../models/IProfiles';
import { ISettingsV2 } from '../../../models/ISettingsV2';
import { LoadingService } from '../../../services/loading.service';
import { EnsureOtpResult, OtpAuthService } from '../../../services/otp-auth.service';
import { SystemService } from '../../../services/system.service';

const NAME_MAX_LENGTH = 20;
const NAME_PATTERN = /^[A-Za-z0-9 _-]*$/;

@Component({
  selector: 'app-profiles',
  templateUrl: './profiles.component.html',
  styleUrls: ['./profiles.component.scss']
})
export class ProfilesComponent implements OnInit, OnDestroy {

  @Input() uri = '';
  // emitted after a profile was activated, the settings form has to reload its values
  @Output() activated = new EventEmitter<void>();

  public readonly slots: number[] = [1, 2, 3];
  public readonly nameMaxLength = NAME_MAX_LENGTH;

  // false as long as the firmware does not know the profiles API, then the card stays hidden
  public loaded = false;
  public active = 0;
  public selected = 1;
  // true if the values the device uses right now differ from the profile activated last
  public modified = false;
  // per profile: do the values the device uses right now equal the profile?
  private liveMatch: { [id: number]: boolean } = {};
  // pool mode the device is configured with right now
  private livePoolMode = 0;
  // true after a profile with another pool mode was activated: the device has to be restarted
  public restartRequired = false;
  public profiles: IProfile[] = [];
  public names: { [id: number]: string } = {};

  // id of the slot that waits for the "really overwrite?" confirmation, 0 = none
  public confirmOverwrite = 0;
  // id of the slot that waits for the "really delete?" confirmation, 0 = none
  public confirmDelete = 0;

  private initialized = false;
  private dirtyNames = new Set<number>();
  private settingsSub?: Subscription;

  constructor(
    private systemService: SystemService,
    private otpAuth: OtpAuthService,
    private loadingService: LoadingService,
    private toastrService: NbToastrService,
    private translate: TranslateService,
  ) { }

  ngOnInit(): void {
    this.load();
    // the settings form below was saved: check again whether the active profile still matches
    this.settingsSub = this.systemService.settingsChanged$.subscribe(() => this.load());
  }

  ngOnDestroy(): void {
    this.settingsSub?.unsubscribe();
  }

  public load(): void {
    this.systemService.getProfiles(this.uri).pipe(
      switchMap((res) =>
        this.systemService.getSettingsV2(this.uri).pipe(map((settings: ISettingsV2) => ({ res, settings })))
      )
    ).subscribe({
      next: ({ res, settings }) => {
        this.active = res.active;
        this.profiles = res.profiles;
        for (const p of res.profiles) {
          // a name the user is typing is not overwritten
          if (!this.dirtyNames.has(p.id)) {
            this.names[p.id] = p.complete ? (p.name ?? '') : '';
          }
        }
        if (!this.initialized) {
          this.selected = res.active || 1;
          this.initialized = true;
        }
        this.modified = this.profileDiffers(this.profileFor(this.active), settings);
        this.livePoolMode = settings.poolMode;
        this.liveMatch = {};
        for (const p of res.profiles) {
          this.liveMatch[p.id] = !!p.complete && !this.profileDiffers(p, settings);
        }
        this.loaded = true;
      },
      error: () => {
        // older firmware: the address returns the web page instead of JSON
        this.loaded = false;
      }
    });
  }

  public select(id: number): void {
    this.selected = id;
    this.confirmOverwrite = 0;
    this.confirmDelete = 0;
  }

  public get current(): IProfile | undefined {
    return this.profileFor(this.selected);
  }

  public profileFor(id: number): IProfile | undefined {
    return this.profiles.find(p => p.id === id);
  }

  public isComplete(id: number): boolean {
    return !!this.profileFor(id)?.complete;
  }

  // name of the profile, or "Profile-<id>" (translated) if it is empty or has no name
  public tabLabel(id: number): string {
    const p = this.profileFor(id);
    if (p?.complete && p.name) {
      return p.name;
    }
    return this.translate.instant('PROFILES.DEFAULT_NAME', { id });
  }

  public fanText(fan: IProfileFan): string {
    switch (fan.mode) {
      case 2: return this.translate.instant('PROFILES.FAN_PID', { temp: fan.pid.targetTemp });
      case 3: return this.translate.instant('PROFILES.FAN_LINKED');
      default: return this.translate.instant('PROFILES.FAN_MANUAL', { speed: fan.manualSpeed });
    }
  }

  public setName(id: number, value: string): void {
    this.names[id] = value ?? '';
    this.dirtyNames.add(id);
  }

  // true if the device already uses exactly the values of this profile
  public matchesLive(id: number): boolean {
    return !!this.liveMatch[id];
  }

  // true if activating this profile changes the pool mode, which needs a restart
  public needsRestart(id: number): boolean {
    const p = this.profileFor(id);
    return !!p?.complete && p.poolMode !== this.livePoolMode;
  }

  // true if the name in the input differs from the stored name
  public nameChanged(id: number): boolean {
    return (this.names[id] ?? '').trim() !== (this.profileFor(id)?.name ?? '');
  }

  public nameValid(id: number): boolean {
    const name = this.names[id] ?? '';
    return name.length <= NAME_MAX_LENGTH && NAME_PATTERN.test(name);
  }

  // Saves the values the device is using right now (not the unsaved form below)
  public saveCurrent(id: number): void {
    if (this.isComplete(id) && this.confirmOverwrite !== id) {
      this.confirmOverwrite = id;
      this.confirmDelete = 0;
      return;
    }
    this.confirmOverwrite = 0;
    this.confirmDelete = 0;

    // the name that is saved, or the default name if the input is empty
    const savedName = (this.names[id] ?? '').trim() || this.translate.instant('PROFILES.DEFAULT_NAME', { id });

    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT')
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          this.systemService.getSettingsV2(this.uri).pipe(
            switchMap((settings: ISettingsV2) =>
              this.systemService.saveProfile(this.uri, id, this.buildProfile(settings, this.names[id] ?? ''), totp)
            ),
            this.loadingService.lockUIUntilComplete()
          )
        ),
      )
      .subscribe({
        next: (res: IProfileSaveResult) => {
          this.dirtyNames.delete(id);
          this.restartRequired = this.restartRequired || !!res?.restartRequired;
          this.toastrService.success(
            this.translate.instant(res?.activated ? 'PROFILES.SAVED_ACTIVATED' : 'PROFILES.SAVED', { name: savedName }),
            this.translate.instant('COMMON.SUCCESS')
          );
          this.load();
        },
        error: (err: HttpErrorResponse) => this.showError(err),
      });
  }

  public cancelConfirm(): void {
    this.confirmOverwrite = 0;
    this.confirmDelete = 0;
  }

  public activate(id: number): void {
    this.confirmOverwrite = 0;
    this.confirmDelete = 0;
    const label = this.tabLabel(id);

    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT')
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          // autoRestart off: the restart is triggered by the user with the button
          this.systemService.activateProfile(this.uri, id, totp, false).pipe(this.loadingService.lockUIUntilComplete())
        ),
      )
      .subscribe({
        next: (res: IProfileActivateResult) => {
          this.restartRequired = this.restartRequired || !!res?.restartRequired;
          this.toastrService.success(
            this.translate.instant('PROFILES.ACTIVATED', { name: label }),
            this.translate.instant('COMMON.SUCCESS')
          );
          this.load();
          this.activated.emit();
        },
        error: (err: HttpErrorResponse) => this.showError(err),
      });
  }

  // Restart of the device, e.g. after a profile with another pool mode was activated (same flow as the settings form)
  public restart(): void {
    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT'),
      { disableOtp: true },
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          this.systemService.restart(this.uri, totp).pipe(
            // drop session on reboot
            tap(() => this.otpAuth.clearSession()),
            this.loadingService.lockUIUntilComplete()
          )
        ),
        catchError(() => {
          this.toastrService.danger(this.translate.instant('SYSTEM.RESTART_FAILED'), this.translate.instant('COMMON.ERROR'));
          return of(null);
        })
      )
      .subscribe(res => {
        if (res !== null) {
          this.restartRequired = false;
          this.toastrService.success(this.translate.instant('SYSTEM.RESTART_SUCCESS'), this.translate.instant('COMMON.SUCCESS'));
        }
      });
  }

  // Removes all values and the name of a profile. The values the device uses right now stay as they are.
  public deleteProfile(id: number): void {
    if (this.confirmDelete !== id) {
      this.confirmDelete = id;
      this.confirmOverwrite = 0;
      return;
    }
    this.confirmDelete = 0;
    const label = this.tabLabel(id);

    this.otpAuth.ensureOtp$(
      this.uri,
      this.translate.instant('SECURITY.OTP_TITLE'),
      this.translate.instant('SECURITY.OTP_HINT')
    )
      .pipe(
        switchMap(({ totp }: EnsureOtpResult) =>
          this.systemService.deleteProfile(this.uri, id, totp).pipe(this.loadingService.lockUIUntilComplete())
        ),
      )
      .subscribe({
        next: () => {
          this.dirtyNames.delete(id);
          this.names[id] = '';
          this.toastrService.success(
            this.translate.instant('PROFILES.DELETED', { name: label }),
            this.translate.instant('COMMON.SUCCESS')
          );
          this.load();
        },
        error: (err: HttpErrorResponse) => this.showError(err),
      });
  }

  // Profile = frequency, core voltage, pool balance and both fan channels.
  // Not part of it: pools, credentials and the overheat temperature.
  private buildProfile(settings: ISettingsV2, name: string): IProfileSave {
    const fans: IProfileFan[] = [];
    for (let ch = 0; ch < 2; ch++) {
      // boards with a single fan: the missing channel is filled with the values of the first one
      const f = settings.fans[ch] ?? settings.fans[0];
      fans.push({
        mode: settings.fans[ch] ? f.mode : 3,
        manualSpeed: f.manualSpeed,
        pid: {
          targetTemp: Math.max(0, f.pid.targetTemp), // -1 = PID not available
          p: f.pid.p,
          i: f.pid.i,
          d: f.pid.d,
        },
      });
    }

    return {
      name: name.trim(),
      frequency: settings.frequency,
      coreVoltage: settings.coreVoltage,
      poolMode: settings.poolMode,
      poolBalance: settings.poolBalance,
      fans,
      activate: true, // the saved profile is the one the device runs with, so it is also the active one
      autoRestart: false, // the restart is triggered by the user with the button
    };
  }

  // Compares what the device uses right now with a profile.
  // Nothing is overwritten automatically, the user decides with "save current values".
  private profileDiffers(p: IProfile | undefined, settings: ISettingsV2): boolean {
    if (!p?.complete) {
      return false;
    }
    // the pool balance only has an effect in dual pool mode
    if (p.frequency !== settings.frequency
      || p.coreVoltage !== settings.coreVoltage
      || p.poolMode !== settings.poolMode
      || (p.poolMode === 1 && p.poolBalance !== settings.poolBalance)) {
      return true;
    }

    const same = (a: number, b: number) => Math.abs(a - b) < 0.005;
    const fans = p.fans ?? [];
    for (let ch = 0; ch < fans.length && ch < settings.fans.length; ch++) {
      const stored = fans[ch];
      const live = settings.fans[ch];
      if (stored.mode !== live.mode
        || stored.manualSpeed !== live.manualSpeed
        || stored.pid.targetTemp !== Math.max(0, live.pid.targetTemp)
        || !same(stored.pid.p, live.pid.p)
        || !same(stored.pid.i, live.pid.i)
        || !same(stored.pid.d, live.pid.d)) {
        return true;
      }
    }
    return false;
  }

  private showError(err: HttpErrorResponse): void {
    // the firmware answers with a short text like "coreVoltage above board limit"
    const detail = typeof err.error === 'string' && err.error ? err.error : err.message;
    this.toastrService.danger(detail, this.translate.instant('COMMON.ERROR'));
  }
}
