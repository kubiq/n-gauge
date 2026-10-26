export type PodWifiEvents = {
  onLost: () => void;
  /** The phone's WiFi was switched on. */
  onWifiOn: () => void;
};
